# QRDecoder DLL 内部逻辑说明

> 本文档与 QRDecoder.cpp / QRDecoder.h 保持同步，如有代码修改请一并更新此文件。

## 一、全局数据结构

DLL 内部有 3 个全局变量，是 7 个函数之间数据共享的核心：

```cpp
static QRDetectorState* g_state = nullptr;           // 检测器对象（ZBar扫描器 + 预处理器）
static std::mutex g_mutex;                            // 线程锁
static std::vector<QRResult> g_lastResults;           // 上次识别结果缓存
```

### QRResult 结构体（每个识别到的二维码）

```cpp
struct QRResult {
    std::string text;      // 二维码文本内容
    int x, y;              // 左上角坐标
    int width, height;     // 矩形宽高
    QRResult() : x(0), y(0), width(0), height(0) {}  // 默认构造
};
```

### QRDetectorState 类（检测器对象）

```cpp
class QRDetectorState {
public:
    bool initialize();        // 创建 ZBar 扫描器 + 预处理器，配置只启用 QR 码
    void release();           // 释放 ZBar 扫描器 + 预处理器
    void scanAtScale(...);    // 在单尺度下扫描图像，坐标除以 scale 缩放回来
    void scanWithConfig(...); // 用一种预处理配置扫描，多尺度（1x~3x）
    bool processFrame(...);   // 多策略处理帧（5策略×5尺度，用于静态图片）
    bool processFrameFast(...); // 快速处理帧（仅灰度+1x/2x，用于摄像头实时）
    bool initialized() const;

private:
    bool initialized_;                  // 是否已初始化
    zbar_image_scanner_t* scanner_;     // ZBar 扫描器
    ImagePreprocessor* preprocessor_;   // 图像预处理器
};
```

### isDuplicateQR 去重函数

扫描结果写入前会检查是否与已有结果重复（文本相同且 IoU > 0.2 的视为重复，跳过）。

### MAX_QR_COUNT

```cpp
#define MAX_QR_COUNT 10   // 单次扫描最多返回 10 个二维码
```

scanAtScale 中 `while (symbol && results.size() < MAX_QR_COUNT)` 限制数量。

---

## 二、7 个函数的角色分类

| 类别 | 函数 | 作用 |
|------|------|------|
| 生命周期 | InitDetector | 创建 g_state，初始化 ZBar |
| 生命周期 | ReleaseDetector | 销毁 g_state，清空 g_lastResults |
| 数据输入 | ProcessImageFile | 读图片文件 → 多策略识别 → 结果存入 g_lastResults + 标注图存文件 |
| 数据输入 | ProcessImageData | 读摄像头数据 → 快速识别 → 结果存入 g_lastResults + 标注图存文件 |
| 数据读取 | GetQRText | 从 g_lastResults 取文本 |
| 数据读取 | GetQRRect | 从 g_lastResults 取坐标 |
| 数据读取 | GetAnnotatedImagePath | 返回标注图文件路径 |

---

## 三、数据流向详解

### 3.1 InitDetector

```
LabVIEW 调用 InitDetector()
    ↓
加锁 std::lock_guard<std::mutex>
    ↓
检查 g_state 是否已存在（重复调用会先释放旧的）
    ↓
new QRDetectorState()
    ↓
g_state->initialize()
    ├── zbar_image_scanner_create()   → 创建 ZBar 扫描器
    ├── ZBar 配置：
    │   ├── ZBAR_QRCODE  → CFG_ENABLE=1      （启用 QR 码）
    │   ├── ZBAR_NONE    → CFG_POSITION=1     （启用位置信息）
    │   ├── ZBAR_CODE128/39/EAN13/EAN8/UPCA/UPCE/I25/DATABAR/DATABAR_EXP/PDF417
    │   │   → CFG_ENABLE=0                     （关闭其他码类型，避免误匹配和 assert 警告）
    │   ├── ZBAR_QRCODE → CFG_X_DENSITY=1     （高密度水平扫描）
    │   └── ZBAR_QRCODE → CFG_Y_DENSITY=1     （高密度垂直扫描）
    └── new ImagePreprocessor()       → 创建预处理器
    ↓
清空 g_lastResults
    ↓
返回 QR_OK(0) 或 QR_ERR_INIT(-1)
```

**写入的全局变量：** g_state, g_lastResults

### 3.2 ProcessImageFile（多策略模式，用于静态图片，追求最高识别率）

```
LabVIEW 传入: inputPath = "D:\xxx.png"
    ↓
加锁 std::lock_guard<std::mutex>
    ↓
检查 g_state 是否存在且已初始化 → 否则返回 QR_ERR_INIT(-1)
检查 inputPath 不为空 → 否则返回 QR_ERR_PROCESS(-3)
    ↓
cv::imread(inputPath) → 读入图片 → frame
    ↓ frame 为空则返回 QR_ERR_PROCESS(-3)
g_state->processFrame(frame, outFrame, results, procTime)
    ├── 小图自动放大（短边<300 → 放大到短边=300）
    ├── 5种预处理策略依次尝试（详见下方），每种多尺度扫描（1x,1.5x,2x,2.5x,3x）
    ├── 任一策略识别成功即停止（goto done）
    ├── 坐标缩放回原始尺寸（如果之前放大了）
    └── 在 outFrame 上画绿框 + 文字（文字截取前30字符）
    ↓
cv::imwrite(标注图路径, outFrame)     → 保存到 D:\...\Temp\qr_annotated.bmp
    ↓
g_lastResults = results               → ★ 识别结果存入全局缓存
    ↓
返回 results.size()（二维码数量，0 表示未识别到）
```

**5种预处理策略的具体配置：**

| 策略 | 灰度 | 高斯模糊 | 自适应二值化 | 形态学 | CLAHE | 锐化 | 适用场景 |
|------|------|----------|-------------|--------|-------|------|----------|
| 1 | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | 标准流程 |
| 2 | ✓ | ✗ | ✓ | ✗ | ✓ | ✓ | 保留细节，适合小图/锐利图 |
| 3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | 最简，高对比度场景 |
| 4 | ✓ | ✓ | ✓ | ✓(3×3,1次) | ✗ | ✗ | 修复断裂码 |
| 5 | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | 灰度直接扫，高质量图 |

**写入的全局变量：** g_lastResults
**写入的文件：** D:\Code\OpenCV\qr_decoder_project\Temp\qr_annotated.bmp
**读取的全局变量：** g_state

### 3.3 ProcessImageData（快速模式，用于摄像头实时数据，避免卡顿）

```
LabVIEW 传入: imageData, width, height, colorOrder
    ↓
加锁 std::lock_guard<std::mutex>
    ↓
检查 g_state 是否存在且已初始化 → 否则返回 QR_ERR_INIT(-1)
检查 imageData 不为空、width/height > 0 → 否则返回 QR_ERR_PROCESS(-3)
    ↓
根据 colorOrder 解析像素数据：
    ├── colorOrder=0: Mat(height, width, CV_8UC3, imageData).clone() → BGR 直接使用
    ├── colorOrder=1: Mat(height, width, CV_8UC3, imageData) + cvtColor(RGB→BGR)
    └── colorOrder=2: 见下方详细流程
    ↓
g_state->processFrameFast(frame, outFrame, results, procTime)
    ├── 仅灰度转换 cvtColor(BGR2GRAY)
    ├── 1x 扫描，若为空再 2x 扫描
    └── 在 outFrame 上画绿框 + 文字
    ↓
cv::imwrite(标注图路径, outFrame)     → 保存到 D:\...\Temp\qr_annotated.bmp
    ↓
g_lastResults = results               → ★ 识别结果存入全局缓存
    ↓
返回 results.size()（二维码数量）
```

**colorOrder=2 的数据转换流程：**

LabVIEW IMAQ ColorImageToArray 输出 2D U32 数组，CLF 配置 Dimensions=2, Array Data Pointer。

- LabVIEW 2D 数组是列优先（column-major），内存中先存第 0 列所有行，再存第 1 列...
- U32 像素格式：0x00RRGGBB（小端内存中：BB GG RR 00 = BGRA）
- 转换步骤：
  1. `Mat(width, height, CV_8UC4, imageData)` — 按行读取，每行是一列数据（因为列优先）
  2. `transpose` — 恢复维度为 height×width
  3. `rotate(ROTATE_90_CLOCKWISE)` — 修正 transpose 带来的方向偏移
  4. `cvtColor(BGRA→BGR)` — 转为 OpenCV 标准 BGR 格式

**写入的全局变量：** g_lastResults
**写入的文件：** D:\Code\OpenCV\qr_decoder_project\Temp\qr_annotated.bmp
**读取的全局变量：** g_state

### 3.4 GetQRText

```
LabVIEW 传入: index, buffer, bufferSize
    ↓
加锁 std::lock_guard<std::mutex>
    ↓
检查 buffer 不为空 且 bufferSize > 0 → 否则返回 QR_ERR_PROCESS(-3)
    ↓
检查 index 在 [0, g_lastResults.size()) 范围内 → 越界返回 QR_ERR_INDEX(-4)
    ↓
从 g_lastResults[index].text 取出字符串
    ↓
copyLen = min(text.size(), bufferSize-1)
memcpy(buffer, text, copyLen)
buffer[copyLen] = '\0'
    ↓
返回 QR_OK(0)
```

**读取的全局变量：** g_lastResults
**不修改任何全局变量**

### 3.5 GetQRRect

```
LabVIEW 传入: index, outX*, outY*, outW*, outH*
    ↓
加锁 std::lock_guard<std::mutex>
    ↓
检查 index 在 [0, g_lastResults.size()) 范围内 → 越界返回 QR_ERR_INDEX(-4)
    ↓
if (outX) *outX = g_lastResults[index].x        // 空指针安全：传入 nullptr 则跳过
if (outY) *outY = g_lastResults[index].y
if (outW) *outW = g_lastResults[index].width
if (outH) *outH = g_lastResults[index].height
    ↓
返回 QR_OK(0)
```

**读取的全局变量：** g_lastResults
**不修改任何全局变量**
**注意：** outX/outY/outW/outH 传入 nullptr 是安全的，不会崩溃

### 3.6 GetAnnotatedImagePath

```
LabVIEW 传入: buffer, bufferSize
    ↓
⚠️ 不加锁（路径是硬编码的，不访问任何全局变量）
    ↓
检查 buffer 不为空 且 bufferSize > 0 → 否则返回 QR_ERR_PROCESS(-3)
    ↓
调用 getAnnotatedPath() → 返回固定路径
    "D:\Code\OpenCV\qr_decoder_project\Temp\qr_annotated.bmp"
    ↓
copyLen = min(path.size(), bufferSize-1)
memcpy(buffer, path, copyLen)
buffer[copyLen] = '\0'
    ↓
返回 QR_OK(0)
```

**不读取也不修改任何全局变量**（路径是硬编码的）
**注意：** 这是唯一不加锁的函数，因为不访问任何共享状态

### 3.7 ReleaseDetector

```
LabVIEW 调用 ReleaseDetector()
    ↓
加锁 std::lock_guard<std::mutex>
    ↓
g_state->release()    → 释放 ZBar 扫描器 + 预处理器
delete g_state
g_state = nullptr
    ↓
g_lastResults.clear() → 清空结果缓存
    ↓
无返回值（void）
```

**写入的全局变量：** g_state, g_lastResults

---

## 四、全局变量在各函数中的读写关系

| 函数 | 加锁 | g_state | g_lastResults | 磁盘文件 |
|------|------|---------|---------------|----------|
| InitDetector | ✓ | **写**（创建） | **写**（清空） | — |
| ProcessImageFile | ✓ | **读**（判断是否初始化） | **写**（存入识别结果） | **写**（保存标注图） |
| ProcessImageData | ✓ | **读**（判断是否初始化） | **写**（存入识别结果） | **写**（保存标注图） |
| GetQRText | ✓ | — | **读**（取文本） | — |
| GetQRRect | ✓ | — | **读**（取坐标） | — |
| GetAnnotatedImagePath | ✗ | — | — | — |
| ReleaseDetector | ✓ | **写**（销毁） | **写**（清空） | — |

---

## 五、LabVIEW 调用顺序和连接逻辑

### 必须的调用顺序

```
InitDetector → Process* → GetQR* / GetAnnotatedImagePath → ReleaseDetector
```

原因：
- `InitDetector` 创建 g_state，不调它，Process* 函数检查 g_state 为空会返回 -1
- `Process*` 把识别结果写入 g_lastResults，不调它，GetQR* 读到的是空的
- `ReleaseDetector` 销毁 g_state，调完后再调 Process* 会返回 -1

### LabVIEW 中数据怎么"传"的

**Process* 和 GetQR* 之间不需要 LabVIEW 连线传数据**，它们通过 DLL 内部的 `g_lastResults` 全局变量共享数据。

LabVIEW 中唯一需要连线的是：
- Process* 的返回值 `qrCount` → For Loop 的 N 端（控制循环次数）
- For Loop 的 `i` → GetQRText 和 GetQRRect 的 `index` 参数

### 完整调用流程

```
1. InitDetector()                    → 返回 0 表示成功
2. ProcessImageFile("D:\xxx.png")    → 返回 qrCount（比如 2）
3. For i = 0 to qrCount-1:
     GetQRText(i, buf, 2048)         → 从 g_lastResults[0] 和 [1] 取文本
     GetQRRect(i, &x, &y, &w, &h)   → 从 g_lastResults[0] 和 [1] 取坐标
4. GetAnnotatedImagePath(buf, 512)   → 返回 "D:\...\Temp\qr_annotated.bmp"
5. 用路径读图片显示
6. ReleaseDetector()                 → 释放所有资源
```

---

## 六、两套处理模式的区别

| | ProcessImageFile（多策略模式） | ProcessImageData（快速模式） |
|---|---|---|
| **用途** | 静态图片识别 | 摄像头实时数据 |
| **处理方法** | processFrame | processFrameFast |
| **预处理** | 5种策略依次尝试（灰度+模糊+二值化+CLAHE+锐化等组合） | 仅灰度转换 |
| **多尺度** | 1x, 1.5x, 2x, 2.5x, 3x（5个尺度） | 1x + 2x（2个尺度，1x有结果则跳过2x） |
| **耗时** | 较慢（追求最高识别率） | 约 20-50ms（摄像头不卡顿） |
| **小图放大** | 短边<300 自动放大 | 不放大 |
| **坐标缩放** | 放大后坐标缩放回原始尺寸 | 无需缩放 |
| **去重** | isDuplicateQR（文本相同+IoU>0.2） | 同左 |
| **结果上限** | MAX_QR_COUNT=10 | 同左 |
| **识别率** | ~93.6%（172张测试图） | 略低，但满足实时场景 |

---

## 七、多次调用的行为

**每次调用 ProcessImageFile / ProcessImageData 都会：**
1. 覆盖 `g_lastResults`（上次识别结果清空，换成新的）
2. 覆盖 `Temp\qr_annotated.bmp`（上次标注图被新图替换）

所以如果连续处理多张图片，必须在每次 Process* 之后立即读取 GetQR* 和 GetAnnotatedImagePath，否则下一次 Process* 会覆盖结果。

---

## 八、线程安全

除 GetAnnotatedImagePath 外，所有函数内部都用 `std::lock_guard<std::mutex>` 加了锁，多线程调用不会崩溃。GetAnnotatedImagePath 不加锁，因为它只读取硬编码路径字符串，不访问任何共享状态。

但 LabVIEW 中一般是单线程顺序调用，不需要关心这个。

---

## 九、两套数据存储的分工

| 数据类型 | 存储位置 | 获取方式 | 用途 |
|----------|----------|----------|------|
| 识别文本 + 坐标 | DLL 内存中的 g_lastResults | GetQRText / GetQRRect | 知道二维码内容和位置 |
| 绿框标注图 | 磁盘文件 Temp\qr_annotated.bmp | GetAnnotatedImagePath 拿路径，再读文件 | 在 LabVIEW 中显示图片 |

---

## 十、LabVIEW CLF 配置要点

### ProcessImageData 的 CLF 配置
- imageData：类型 U32 Array，Dimensions=2，Array Data Pointer
- width/height：int32 传值
- colorOrder：int32 传值，传 2 表示 U32 格式
- **绝不能用 Reshape Array 把 2D 数组变 1D**，会破坏数据指针
- IMAQ ColorImageToArray 输出的 2D U32 数组直接连 CLF

### 标注图显示
- 调用 GetAnnotatedImagePath 获取路径字符串
- 用 IMAQ Read File 读取该 BMP 路径
- BMP 格式写入极快（无压缩），适合摄像头 30fps 场景

---

## 十一、返回值定义

| 常量 | 值 | 含义 |
|------|----|------|
| QR_OK | 0 | 成功 |
| QR_ERR_INIT | -1 | 未初始化或初始化失败 |
| QR_ERR_PROCESS | -3 | 处理失败（参数无效、图片为空等） |
| QR_ERR_INDEX | -4 | 索引越界（GetQRText/GetQRRect 的 index 超出范围） |

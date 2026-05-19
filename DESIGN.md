# 基于LabVIEW的二维码图像识别系统 — 设计文档

> 文档版本：v2.0 | 2026-05-04
> 与 QRDecoder.cpp / QRDecoder.h 同步维护

***

## 目录

1. [设计目标](#1-设计目标)
2. [架构设计](#2-架构设计)
3. [模块职责与边界](#3-模块职责与边界)
4. [数据流设计](#4-数据流设计)
5. [接口设计](#5-接口设计)
6. [预处理流水线设计](#6-预处理流水线设计)
7. [关键设计决策与理由](#7-关键设计决策与理由)
8. [设计约束](#8-设计约束)
9. [多二维码识别设计](#9-多二维码识别设计)
10. [线程安全设计](#10-线程安全设计)

***

## 1. 设计目标

### 1.1 核心需求

| # | 需求 | 优先级 |
|---|------|--------|
| 1 | 二维码图像识别功能 | P0 |
| 2 | 与 LabVIEW 的无缝集成 | P0 |
| 3 | 支持多种图像输入源（单张图片、摄像头） | P1 |
| 4 | 多二维码同时识别与位置标注 | P1 |
| 5 | 摄像头实时模式不卡顿 | P1 |

### 1.2 设计原则

- **职责分离**：DLL 只负责识别，LabVIEW 负责所有上层逻辑和 UI
- **C 接口隔离**：所有导出函数使用纯 C 类型（int, char*），消除 C++ ABI 兼容性问题
- **CLF 友好**：每个函数最多 5 个参数，避免 LabVIEW CLF 配置困难
- **零外部依赖**：ZBar 静态链接进 DLL，减少部署复杂度
- **两套处理模式**：静态图片走多策略高识别率，摄像头走快速路径保流畅

***

## 2. 架构设计

### 2.1 分层架构

```
┌──────────────────────────────────────────────────┐
│              LabVIEW 应用层                        │
│  流程控制 · UI 界面 · 图像显示 · 数据展示           │
└──────────────────┬───────────────────────────────┘
                   │ CLF Node (cdecl 调用约定)
                   │ 纯 C 类型接口
┌──────────────────┴───────────────────────────────┐
│              DLL 接口层 (extern "C")               │
│  InitDetector · ProcessImageFile · ProcessImageData│
│  GetQRText · GetQRRect · GetAnnotatedImagePath    │
│  ReleaseDetector                                  │
└──────────────────┬───────────────────────────────┘
                   │
┌──────────────────┴───────────────────────────────┐
│              核心处理层                             │
│  QRDetectorState (内部类)                          │
│  ├── ZBar ImageScanner    — 二维码解码             │
│  ├── ImagePreprocessor    — 图像预处理流水线         │
│  ├── processFrame         — 多策略处理（静态图片）   │
│  └── processFrameFast     — 快速处理（摄像头实时）   │
└──────────────────┬───────────────────────────────┘
                   │
┌──────────────────┴───────────────────────────────┐
│              基础设施层                             │
│  OpenCV 4.14.0  ·  ZBar 0.23.93  ·  clock_compat  │
└──────────────────────────────────────────────────┘
```

### 2.2 架构选型理由

| 决策 | 选项 | 选择 | 理由 |
|------|------|------|------|
| 整体架构 | 纯 LabVIEW / 纯 C++ / **混合架构** | 混合架构 | LabVIEW 擅长 UI 和流程控制，C++ 擅长图像处理；分工明确，答辩时可展示 LabVIEW 工程能力 |
| 解码库 | OpenCV QRCodeDetector / **ZBar** | ZBar | OpenCV 4.14.0 未编译 QUIRC 模块，QRCodeDetector 不可用；ZBar 成熟稳定 |
| ZBar 链接方式 | 动态链接 / **静态链接** | 静态链接 | 消除运行时 DLL 依赖，简化部署 |
| DLL 接口风格 | C++ 类 / **C 函数** | C 函数 | LabVIEW CLF 只能调用 C 风格函数；避免 C++ name mangling 和 ABI 问题 |
| 结果传递方式 | 输出参数 / **全局缓存 + 按索引读取** | 全局缓存 | 减少参数数量，每个函数最多 5 参数，CLF 配置简单 |
| 标注图传递 | 内存数据 / **文件路径** | 文件路径 | LabVIEW 用 IMAQ Read File 读 BMP，避免大量数据通过 CLF 传递 |

***

## 3. 模块职责与边界

### 3.1 模块职责矩阵

| 模块 | 文件 | 职责 | 不负责 |
|------|------|------|--------|
| **DLL 导出层** | QRDecoder.h/cpp | 导出 C 接口，状态管理，线程安全 | 图像处理算法 |
| **图像预处理** | ImagePreprocessor.h/cpp | 灰度化、滤波、二值化、CLAHE、锐化、形态学 | 二维码解码逻辑 |
| **兼容层** | clock_compat.c | 提供 clock_gettime64 实现 | 业务逻辑 |
| **LabVIEW 前端** | (LabVIEW VI) | UI、流程控制、图像显示 | 图像处理 |

### 3.2 QRDetectorState 内部类设计

```
QRDetectorState
├── scanner_: zbar_image_scanner_t*    // ZBar 扫描器实例
├── preprocessor_: ImagePreprocessor*  // 预处理器实例
├── initialized_: bool                 // 是否已初始化
│
├── 公共方法:
│   ├── initialize()        → 创建 ZBar 扫描器 + 预处理器，配置只启用 QR 码
│   ├── release()           → 释放资源
│   ├── scanAtScale(...)    → 单尺度扫描，坐标除以 scale 缩放回来
│   ├── scanWithConfig(...) → 一种预处理配置 + 多尺度扫描
│   ├── processFrame(...)   → 5策略×5尺度（静态图片，高识别率）
│   └── processFrameFast(...) → 仅灰度+1x/2x（摄像头实时，低延迟）
```

**设计意图**：将所有可变状态封装在单一类中，全局仅一个实例（通过 `g_state` 指针管理），导出的 C 函数通过该实例操作，避免全局变量散落。

### 3.3 ImagePreprocessor 模块设计

```
ImagePreprocessor
├── 开关状态 (6个 bool)
│   ├── enableGrayConvert    = true
│   ├── enableGaussianBlur   = true
│   ├── enableAdaptiveBinary = true
│   ├── enableMorphology     = false
│   ├── enableCLAHE          = false
│   └── enableSharpen        = false
│
├── 算法参数 (5个)
│   ├── gaussianKernelSize   = 5
│   ├── adaptiveBlockSize    = 11
│   ├── adaptiveC            = 2
│   ├── morphologyKernelSize = 3
│   └── morphologyIterations = 1
│
└── process(input, output) → 按开关状态执行流水线
```

**设计意图**：每个步骤独立开关，允许任意组合。`process()` 内部按固定顺序执行已启用的步骤，跳过已禁用的步骤。

***

## 4. 数据流设计

### 4.1 静态图片模式数据流

```
图片文件路径
  │
  ▼
cv::imread(inputPath) ──→ BGR 帧 (cv::Mat)
  │
  ├──────────────────────────────┐
  │                              │
  ▼                              ▼
原始帧 (copyTo)             processFrame:
  │                              │
  │                         小图自动放大(短边<300)
  │                              │
  │                         5种预处理策略依次尝试
  │                         每种策略5个尺度(1x~3x)
  │                              │
  │                         ZBar 扫描
  │                              │
  │                         去重(IoU>0.2)
  │                         坐标缩放回原始尺寸
  │                              │
  ▼                              ▼
在原始帧上绘制标注             收集结果 → g_lastResults
(绿框 + 文本前30字符)          (文本 + 包围盒)
  │                              │
  └──────────┬───────────────────┘
             │
             ▼
    cv::imwrite → Temp\qr_annotated.bmp
             │
             ▼
    LabVIEW: GetAnnotatedImagePath → IMAQ Read File → 显示
```

### 4.2 摄像头模式数据流

```
LabVIEW IMAQ ColorImageToArray
  │
  ▼
imageData (2D U32数组) + width + height + colorOrder=2
  │
  ▼
ProcessImageData()
  │
  ├── colorOrder == 0? → BGR 直接使用
  ├── colorOrder == 1? → cvtColor(RGB → BGR)
  └── colorOrder == 2? → Mat(width,height,CV_8UC4)
                         → transpose → rotate(90°CW)
                         → cvtColor(BGRA → BGR)
  │
  ▼
processFrameFast():
  ├── 仅灰度转换 cvtColor(BGR2GRAY)
  ├── 1x 扫描，若为空再 2x 扫描
  └── 画绿框 + 文本
  │
  ▼
cv::imwrite → Temp\qr_annotated.bmp
  │
  ▼
LabVIEW: GetAnnotatedImagePath → IMAQ Read File → 显示
```

**设计决策**：增加 `colorOrder` 参数而非强制转换。理由是 OpenCV 来源的图像已经是 BGR，无需额外转换；IMAQdx 来源是 RGB，需要转换；IMAQ ColorImageToArray 输出 U32，需要 BGRA→BGR。显式参数避免无谓的性能开销。

### 4.3 结果数据读取

```
Process* 返回 qrCount (>=0)
     │
     ▼
For i = 0 to qrCount-1:
     GetQRText(i, buf, 2048)     → 从 g_lastResults[i] 取文本
     GetQRRect(i, &x, &y, &w, &h) → 从 g_lastResults[i] 取坐标
```

**设计决策**：使用全局缓存 + 按索引读取，而非将所有结果通过函数参数返回。理由是 LabVIEW CLF 处理大量输出参数非常痛苦（参数越多越容易配错），7个函数每个最多5参数的方案远比3函数16参数的方案好配置。

***

## 5. 接口设计

### 5.1 设计原则

1. **纯 C 类型**：参数仅使用 `int`, `char*`, `unsigned char*`，避免结构体和 C++ 类型
2. **cdecl 调用约定**：LabVIEW CLF 的默认且推荐约定
3. **预分配缓冲区**：所有输出缓冲区由调用方（LabVIEW）预分配，DLL 只填充数据
4. **错误码返回**：所有函数返回 `int`，0=成功，正数=二维码数量，负数=错误
5. **每个函数最多5参数**：避免 LabVIEW CLF 配置困难

### 5.2 接口分类

| 类别 | 函数 | 说明 |
|------|------|------|
| **生命周期** | `InitDetector`, `ReleaseDetector` | 创建/销毁检测器实例 |
| **数据输入** | `ProcessImageFile` | 处理图片文件（多策略模式） |
| **数据输入** | `ProcessImageData` | 处理摄像头数据（快速模式） |
| **结果读取** | `GetQRText`, `GetQRRect` | 按索引读取识别结果 |
| **辅助** | `GetAnnotatedImagePath` | 获取标注图文件路径 |

### 5.3 返回码设计

```
返回值语义:
  >= 0    → Process* 函数：检测到的二维码数量
  0       → 其他函数：成功 (QR_OK)
  -1      → 初始化失败 (QR_ERR_INIT)
  -3      → 处理错误 (QR_ERR_PROCESS)
  -4      → 索引越界 (QR_ERR_INDEX)
```

**设计决策**：Process* 函数用正数返回二维码数量，既节省参数又直观。GetQRText/GetQRRect 返回 0 表示成功，-4 表示索引越界。

***

## 6. 预处理流水线设计

### 6.1 流水线顺序（processFrame 内部使用的完整流水线）

```
BGR 输入
  │
  ▼ Step 1: 灰度化 (cvtColor BGR→GRAY)
  │         ZBar 要求灰度输入，这是必需步骤
  ▼
Step 2: 高斯滤波 (GaussianBlur, kernel=5)
  │         去除高斯噪声，平滑图像
  ▼
Step 3: 自适应二值化 (adaptiveThreshold, GAUSSIAN_C, block=11, C=2)
  │         处理光照不均匀场景
  ▼
Step 4: CLAHE 对比度增强 (可选)
  │         增强低对比度图像的细节
  ▼
Step 5: 锐化 (可选)
  │         增强边缘信息
  ▼
Step 6: 形态学处理 (开运算 + 闭运算, kernel=3)
  │         去除小噪点 + 填充小空洞
  ▼
输出 (cv::Mat 灰度图，送入 ZBar)
```

### 6.2 5种预处理策略

processFrame 内部依次尝试 5 种策略，任一策略识别成功即停止：

| 策略 | 灰度 | 高斯模糊 | 自适应二值化 | 形态学 | CLAHE | 锐化 | 适用场景 |
|------|------|----------|-------------|--------|-------|------|----------|
| 1 | ✓ | ✓ | ✓ | ✗ | ✓ | ✓ | 标准流程 |
| 2 | ✓ | ✗ | ✓ | ✗ | ✓ | ✓ | 保留细节，适合小图/锐利图 |
| 3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | 最简，高对比度场景 |
| 4 | ✓ | ✓ | ✓ | ✓(3×3,1次) | ✗ | ✗ | 修复断裂码 |
| 5 | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | 灰度直接扫，高质量图 |

### 6.3 快速模式（processFrameFast）

摄像头实时场景不使用多策略，仅做：
1. `cvtColor(BGR2GRAY)` — 灰度转换
2. `scanAtScale(gray, 1.0)` — 原尺度扫描
3. 若无结果，`scanAtScale(gray, 2.0)` — 2倍放大扫描

耗时约 20-50ms，满足 30fps 不卡顿。

***

## 7. 关键设计决策与理由

### 7.1 为什么用混合架构而不是纯 LabVIEW？

| | 纯 LabVIEW | 混合架构 (LabVIEW + DLL) |
|---|---|---|
| 图像处理能力 | 有限（需 IMAQ Vision 付费工具包） | 强大（OpenCV 全功能） |
| 二维码解码 | 无原生支持 | ZBar 成熟方案 |
| 答辩展示 | LabVIEW 仅是壳 | LabVIEW 承担实际业务逻辑 |
| 开发效率 | 图像算法开发慢 | C++ 图像算法快速实现 |

**结论**：混合架构在功能、性能、答辩展示三个维度都占优。

### 7.2 为什么 ZBar 而不是 OpenCV QRCodeDetector？

OpenCV 4.14.0 默认构建不包含 QUIRC 模块（QR 解码器），`QRCodeDetector` 可以定位但无法解码。重新编译 OpenCV 并启用 QUIRC 工作量大，而 ZBar 是成熟、轻量的替代方案，静态链接后无额外运行时依赖。

### 7.3 为什么使用静态链接 ZBar？

| | 动态链接 (libzbar.dll) | 静态链接 (libzbar.a) |
|---|---|---|
| 部署文件数 | +1 DLL | 不变 |
| 运行时查找 | 需确保 DLL 在 PATH 中 | 无需 |
| 体积影响 | 无 | QRDecoder.dll 增大约 300KB |
| 风险 | DLL 版本不匹配/丢失 | 无 |

**结论**：静态链接以极小的体积代价换取零运行时依赖，对 LabVIEW 集成场景最优。

### 7.4 为什么用全局缓存 + 按索引读取而不是输出参数？

| | 输出参数 | 全局缓存 + 按索引读取 |
|---|---|---|
| Process* 函数参数数量 | 13-16 个 | 4 个 |
| CLF 配置难度 | 极高（16参数极易配错） | 低（最多5参数） |
| LabVIEW 端复杂度 | 低（一次取回所有数据） | 略高（需 For Loop + GetQR*） |
| 出错概率 | 高 | 低 |

**结论**：全局缓存方案将 16 参数简化为 7 个函数各 5 参数以内，CLF 配置从"几乎不可能"变为"轻松"。

### 7.5 为什么标注图通过文件路径而不是内存数据返回？

| | 内存数据 (outImageData) | 文件路径 (BMP) |
|---|---|---|
| CLF 参数增加 | +2 (data ptr + size) | 0 (用 GetAnnotatedImagePath 单独获取) |
| LabVIEW 显示 | 需要手动 reshape + 色彩转换 | IMAQ Read File 一键读取 |
| 性能 | 需要复制大量数据 | BMP 写入极快（无压缩），IMAQ 读取也快 |
| 兼容性 | 色彩格式/行对齐问题多 | BMP 标准格式，无歧义 |

**结论**：文件路径方案更简单、更可靠、性能也不差。BMP 格式无压缩，写入速度远快于 PNG。

### 7.6 为什么标注绘制在原始帧而非预处理帧上？

预处理帧可能是灰度图或二值图，色彩信息已丢失，不适合展示给用户。在原始 BGR 帧上绘制绿框和文本，用户看到的是与输入一致的彩色图像加上识别标注。

***

## 8. 设计约束

### 8.1 技术约束

| 约束 | 影响 | 缓解措施 |
|------|------|----------|
| LabVIEW 必须是 64-bit | 32-bit LabVIEW 无法调用 64-bit DLL | 文档明确标注要求 |
| OpenCV putText 不支持中文 | 标注中的中文显示为方块 | 中文内容由 LabVIEW 端文本控件展示 |
| 单帧最多 10 个二维码 | MAX_QR_COUNT 硬编码 | 覆盖绝大多数实际场景，可按需修改 |
| LabVIEW 2D 数组是列优先 | colorOrder=2 需 transpose + rotate | ProcessImageData 内部自动处理 |
| IMAQ ColorImageToArray 输出 U32 | CLF 必须配置 Dimensions=2 | 文档明确说明 CLF 配置方式 |
| Reshape Array 破坏数据指针 | 不能用 Reshape Array 转换 2D→1D | CLF 直接接受 2D 数组 |

### 8.2 编译约束

| 约束 | 原因 |
|------|------|
| 必须使用 MinGW-w64 (GCC) | 与 OpenCV 编译工具链一致，避免运行时兼容问题 |
| 需要 clock_compat.c 兼容层 | MSYS2 预编译 libzbar.a 引用 clock_gettime64，GCC 15 CRT 已移除该符号 |
| libwinpthread-1.dll 必须来自 MinGW | MSYS2 版本 ABI 不兼容 |

### 8.3 运行时约束

| 约束 | 说明 |
|------|------|
| DLL 不是线程安全的并行处理 | 内部 mutex 保证串行访问，不支持多线程同时调用 |
| 单实例模式 | 同一时刻只能有一个检测器实例 |
| g_lastResults 每次覆盖 | 连续调用 Process* 必须在每次后立即读取结果 |

***

## 9. 多二维码识别设计

### 9.1 扫描与去重

```
ZBar 扫描 → 多个符号 (symbol)
     │
     ▼
遍历每个符号:
     ├── 提取文本
     ├── 计算包围盒 (多边形顶点 → axis-aligned bbox)
     ├── 坐标除以 scale 缩放回原始尺寸
     └── 去重检查: 与已有结果比较
         ├── 文本不同 → 不重复
         └── 文本相同 + IoU > 0.2 → 重复，跳过
```

### 9.2 包围盒计算

```
ZBar 符号多边形顶点 → 包围盒:
  遍历所有顶点 (x, y):
    minX = min(所有 x)
    minY = min(所有 y)
    maxX = max(所有 x)
    maxY = max(所有 y)

  包围盒:
    x = minX
    y = minY
    width = maxX - minX
    height = maxY - minY
```

### 9.3 标注绘制规则

- 每个检测到的二维码：绿色矩形框 (`cv::rectangle`, 颜色 Scalar(0,255,0), 线宽 2) + 文本标注 (`cv::putText`)
- 文本标注截取前 30 个字符，位于矩形框上方
- 标注绘制在原始 BGR 帧上（非预处理帧）

***

## 10. 线程安全设计

### 10.1 策略

```
全局状态:
  QRDetectorState* g_state = nullptr
  std::mutex g_mutex
  std::vector<QRResult> g_lastResults

每个导出函数（除 GetAnnotatedImagePath 外）:
  {
      std::lock_guard<std::mutex> lock(g_mutex);
      // ... 操作 g_state / g_lastResults ...
  }

GetAnnotatedImagePath 不加锁:
  // 只读取硬编码路径字符串，不访问任何共享状态
```

### 10.2 设计意图

- LabVIEW 的生产者-消费者模式中，可能存在并发调用 DLL 的情况
- 使用 `std::mutex` + `std::lock_guard` 保证所有导出函数互斥执行
- 这意味着处理是串行的——如果需要并行处理多路视频，需要运行多个 LabVIEW 进程

***

> 📝 本文档聚焦于设计决策与架构理由，具体操作指南（编译、部署、测试）请参考 `PROJECT_DETAIL.md`。

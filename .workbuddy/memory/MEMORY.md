# QR Decoder Project - 长期记忆

## 项目概述
- 毕业设计：基于LabVIEW的二维码图像识别系统
- 架构：LabVIEW前端 + C++ DLL后端 (OpenCV 4.14.0 + ZBar 0.23.93)
- 编译工具链：MinGW-w64 GCC 15 x64，bash 中直接用 g++ 编译

## 关键技术决策
- API v2：7 个函数（每个最多 5 参数），替代原来 3 函数 16 参数版本
- 函数列表：InitDetector / ProcessImageFile / ProcessImageData / GetQRText / GetQRRect / GetAnnotatedImagePath / ReleaseDetector
- ProcessImageFile(inputPath) 返回二维码数量，GetQRText(index) / GetQRRect(index) 按索引取结果
- 标注图自动保存到 D:\Code\OpenCV\qr_decoder_project\Temp\qr_annotated.bmp（BMP格式，写入极快）
- LabVIEW 用 IMAQ Read File 读标注图路径显示绿框
- OpenCV 4.14.0 未编译 QUIRC 模块，QRCodeDetector 不可用，改用 ZBar
- ZBar 静态链接进 DLL（libzbar.a），避免运行时依赖
- clock_compat.c 解决 GCC 15 CRT 中 clock_gettime64 符号缺失问题
- colorOrder 参数：0=BGR(3字节), 1=RGB(3字节), 2=U32(4字节, IMAQ ColorImageToArray)
- LabVIEW 2D数组是列优先(column-major)，colorOrder=2时需用 Mat(width,height) + transpose
- ProcessImageData 走 processFrameFast（仅灰度+1x/2x扫描），摄像头实时不卡顿
- ProcessImageFile 走 processFrame（5策略×5尺度多策略），追求最高识别率
- 多策略预处理（5种配置并行尝试）+ 小图自动放大 + 只启用 QR 码类型 → 识别率 93.6%
- ZBar 关闭非 QR 码类型（databar 等），避免误匹配和 assert 警告

## 代码修改历史
- 2026-04-25: 添加 USE_OPENCV_DISPLAY 条件编译，支持摄像头模式和窗口显示
- 2026-04-25: ProcessImageData 新增 colorOrder 参数，自动 RGB→BGR 转换
- 2026-04-25: 添加 --dir 批量图片目录测试模式
- 2026-04-25: 添加 SetEnhanceConfig / SetMultiScaleConfig 增强 API（CLAHE+锐化+多尺度）
- 2026-04-25: 可视化改为运行时 --display 开关，不再需要重编译；build.bat 简化
- 2026-04-25: 测试目录 TestData\Pictures，172 张图片，当前识别率 68%
- 2026-04-26: outImageData 输出改为 RGB 格式（内部 BGR→RGB 转换），LabVIEW 显示颜色不再偏蓝
- 2026-04-26: API 从 15 个函数精简为 3 个（InitDetector / ProcessImage / ReleaseDetector）
- 2026-04-26: ProcessImage 万能函数，同时支持文件路径和摄像头数据，内置保存标注图到文件
- 2026-04-28: API v2 重构为 7 个函数，每个最多 5 参数，解决 CLF 16 参数配置困难
- 2026-04-28: 标注图自动保存到固定路径，不再需要 outputPath 参数
- 2026-04-26: 多策略预处理(5种并行)+小图自动放大+只启用QR码，识别率升至 93.6%
- 2026-04-26: LabVIEW 显示标注图方案：ProcessImage → IMAQ Read File → Image Display
- 2026-05-03: 修复 ProcessImageData 转置维度bug（Mat(height,width)→Mat(width,height)+transpose）
- 2026-05-03: 新增 processFrameFast 方法，ProcessImageData 走快速路径解决摄像头卡顿
- 2026-05-03: 标注图格式从 PNG 改为 BMP，消除压缩延迟

## LabVIEW CLF 关键配置
- ProcessImageData: imageData 配置为 U32 Array, Dimensions=2, Array Data Pointer
- Reshape Array 会破坏数据指针，绝不能用
- IMAQ ColorImageToArray 输出 2D U32 数组，直接连 CLF
- colorOrder=2 表示 U32 格式

## 注意事项
- libwinpthread-1.dll 必须来自 MinGW（D:\MinGW\mingw64\bin），不能用 MSYS2 版本
- LabVIEW 必须 64-bit 才能调用 64-bit DLL
- IMAQdx 默认输出 RGB，传 colorOrder=1 给 DLL
- IMAQ ColorImageToArray 输出 U32，传 colorOrder=2，CLF 用 Dimensions=2

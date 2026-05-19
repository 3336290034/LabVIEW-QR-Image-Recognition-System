/**
* QRDecoder.h - 二维码识别 DLL 导出接口 (v2 - 简化CLF配置版)
*
* 7 个函数，每个最多 5 个参数，LabVIEW CLF 配置轻松：
*   InitDetector()          — 初始化
*   ProcessImageFile(path)  — 处理图片文件
*   ProcessImageData(...)   — 处理摄像头数据
*   GetQRText(idx, buf)     — 取第 idx 个二维码文本
*   GetQRRect(idx, ...)     — 取第 idx 个二维码坐标
*   GetAnnotatedImagePath(buf) — 获取标注图路径
*   ReleaseDetector()       — 释放资源
*
* 标注图（绿框）自动保存到 D:\Code\OpenCV\qr_decoder_project\Temp\qr_annotated.bmp
* LabVIEW 用 IMAQ Read File 读取该路径即可显示
*/

#ifndef QR_DECODER_H
#define QR_DECODER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 返回值定义 ===== */
#define QR_OK              0
#define QR_ERR_INIT       -1
#define QR_ERR_PROCESS    -3
#define QR_ERR_INDEX      -4   /* 索引越界 */

/* ===== 最大二维码数量 ===== */
#define MAX_QR_COUNT      10

/**
 * 初始化检测器（最先调用）
 * @return QR_OK=成功, 负数=失败
 */
__declspec(dllexport) int InitDetector();

/**
 * 处理图片文件
 *
 * 处理流程：多策略预处理 → 多尺度扫描 → 画绿框 → 保存标注图
 *
 * @param inputPath     [输入] 图片文件路径 (C String)
 * @return 检测到的二维码数量 (>=0), 负数=失败
 */
__declspec(dllexport) int ProcessImageFile(const char* inputPath);

/**
 * 处理摄像头图像数据
 *
 * @param imageData     [输入] 像素数据 (CLF配置为U32 Array, Dimensions=2, Array Data Pointer)
 * @param width         [输入] 图像宽度
 * @param height        [输入] 图像高度
 * @param colorOrder    [输入] 0=BGR(3字节), 1=RGB(3字节), 2=U32(4字节, IMAQ ColorImageToArray, CLF Dimensions=2)
 * @return 检测到的二维码数量 (>=0), 负数=失败
 */
__declspec(dllexport) int ProcessImageData(
    unsigned char* imageData,
    int width,
    int height,
    int colorOrder
);

/**
 * 获取第 index 个二维码的文本内容
 *
 * @param index         [输入] 二维码索引 (0 ~ qrCount-1)
 * @param buffer        [输出] 文本缓冲区 (预分配，建议 2048 字节)
 * @param bufferSize    [输入] 缓冲区大小 (建议 2048)
 * @return QR_OK=成功, QR_ERR_INDEX=索引越界
 */
__declspec(dllexport) int GetQRText(
    int index,
    char* buffer,
    int bufferSize
);

/**
 * 获取第 index 个二维码的矩形坐标
 *
 * @param index         [输入] 二维码索引 (0 ~ qrCount-1)
 * @param outX          [输出] 左上角 X 坐标
 * @param outY          [输出] 左上角 Y 坐标
 * @param outW          [输出] 宽度
 * @param outH          [输出] 高度
 * @return QR_OK=成功, QR_ERR_INDEX=索引越界
 */
__declspec(dllexport) int GetQRRect(
    int index,
    int* outX,
    int* outY,
    int* outW,
    int* outH
);

/**
 * 获取标注图保存路径（固定为 D:\Code\OpenCV\qr_decoder_project\Temp\qr_annotated.bmp）
 *
 * LabVIEW 调用此函数获取路径，再用 IMAQ Read File 读取显示
 *
 * @param buffer        [输出] 路径缓冲区 (预分配，建议 512 字节)
 * @param bufferSize    [输入] 缓冲区大小
 * @return QR_OK=成功
 */
__declspec(dllexport) int GetAnnotatedImagePath(
    char* buffer,
    int bufferSize
);

/**
 * 释放所有资源（最后调用）
 */
__declspec(dllexport) void ReleaseDetector();

#ifdef __cplusplus
}
#endif

#endif // QR_DECODER_H

/**
* QRDecoder.cpp - 二维码识别 DLL 核心实现 (v2 - 简化CLF配置版)
*
* 7 个导出函数，每个最多 5 个参数：
*   InitDetector / ProcessImageFile / ProcessImageData
*   GetQRText / GetQRRect / GetAnnotatedImagePath / ReleaseDetector
*
* 标注图自动保存到固定路径（BMP格式，写入速度远快于PNG）
*
* 识别率优化策略（与 v1 完全一致，不动）：
*   - 自动判断图片大小，小图自动放大到足够尺寸
*   - 多种预处理策略并行尝试
*   - 多尺度扫描（1x ~ 3x）
*   - 任一策略识别成功即返回
*/

#include "QRDecoder.h"
#include "ImagePreprocessor.h"

#include <opencv2/opencv.hpp>
#include <zbar.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace zbar;

/* ===== 二维码检测结果结构体 ===== */
struct QRResult {
    std::string text;
    int x, y, width, height;
    QRResult() : x(0), y(0), width(0), height(0) {}
};

/* ===== 去重辅助函数 ===== */
static bool isDuplicateQR(const QRResult& a, const QRResult& b) {
    if (a.text != b.text) return false;
    int areaA = a.width * a.height;
    int areaB = b.width * b.height;
    if (areaA <= 0 || areaB <= 0) return a.text == b.text;
    int overlapX = std::max(0, std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x));
    int overlapY = std::max(0, std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y));
    int overlapArea = overlapX * overlapY;
    float iou = (float)overlapArea / (float)(areaA + areaB - overlapArea);
    return iou > 0.2f;
}

/* ===== 获取标注图保存路径 ===== */
static std::string getAnnotatedPath() {
    return "D:\\Code\\OpenCV\\qr_decoder_project\\Temp\\qr_annotated.bmp";
}

/* ===== 内部状态类 ===== */
class QRDetectorState {
public:
    QRDetectorState()
        : initialized_(false)
        , scanner_(nullptr)
        , preprocessor_(nullptr)
    {}

    bool initialize() {
        if (initialized_) return true;

        scanner_ = zbar_image_scanner_create();
        if (!scanner_) return false;

        // ZBar 基础配置：只启用 QR 码，关闭其他码类型（避免误匹配和 assert 警告）
        zbar_image_scanner_set_config(scanner_, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);
        zbar_image_scanner_set_config(scanner_, ZBAR_NONE, ZBAR_CFG_POSITION, 1);
        // 关闭其他码类型
        zbar_image_scanner_set_config(scanner_, ZBAR_CODE128, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_CODE39, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_EAN13, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_EAN8, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_UPCA, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_UPCE, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_I25, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_DATABAR, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_DATABAR_EXP, ZBAR_CFG_ENABLE, 0);
        zbar_image_scanner_set_config(scanner_, ZBAR_PDF417, ZBAR_CFG_ENABLE, 0);
        // 高密度扫描
        zbar_image_scanner_set_config(scanner_, ZBAR_QRCODE, ZBAR_CFG_X_DENSITY, 1);
        zbar_image_scanner_set_config(scanner_, ZBAR_QRCODE, ZBAR_CFG_Y_DENSITY, 1);

        preprocessor_ = new ImagePreprocessor();

        initialized_ = true;
        return true;
    }

    void release() {
        if (scanner_) {
            zbar_image_scanner_destroy(scanner_);
            scanner_ = nullptr;
        }
        if (preprocessor_) {
            delete preprocessor_;
            preprocessor_ = nullptr;
        }
        initialized_ = false;
    }

    /**
     * 在单尺度下扫描图像，返回检测到的二维码
     */
    void scanAtScale(const cv::Mat& preprocessed, double scale,
                     std::vector<QRResult>& results) {
        cv::Mat scanImg;
        if (std::abs(scale - 1.0) > 0.01) {
            cv::resize(preprocessed, scanImg, cv::Size(), scale, scale, cv::INTER_LINEAR);
        } else {
            scanImg = preprocessed;
        }

        zbar_image_t* image = zbar_image_create();
        zbar_image_set_format(image, *(unsigned long*)"Y800");
        zbar_image_set_size(image, scanImg.cols, scanImg.rows);
        zbar_image_set_data(image, scanImg.data,
                             scanImg.cols * scanImg.rows * sizeof(unsigned char),
                             NULL);

        int n = zbar_scan_image(scanner_, image);
        if (n > 0) {
            const zbar_symbol_t* symbol = zbar_image_first_symbol(image);
            while (symbol && results.size() < MAX_QR_COUNT) {
                if (zbar_symbol_get_type(symbol) != ZBAR_NONE) {
                    QRResult qr;
                    qr.text = std::string(zbar_symbol_get_data(symbol),
                                          zbar_symbol_get_data_length(symbol));

                    unsigned int pointCount = zbar_symbol_get_loc_size(symbol);
                    if (pointCount >= 4) {
                        int minX = INT_MAX, minY = INT_MAX;
                        int maxX = INT_MIN, maxY = INT_MIN;
                        for (unsigned int i = 0; i < pointCount; i++) {
                            int px = (int)(zbar_symbol_get_loc_x(symbol, i) / scale);
                            int py = (int)(zbar_symbol_get_loc_y(symbol, i) / scale);
                            minX = std::min(minX, px);
                            minY = std::min(minY, py);
                            maxX = std::max(maxX, px);
                            maxY = std::max(maxY, py);
                        }
                        qr.x = minX;
                        qr.y = minY;
                        qr.width = maxX - minX;
                        qr.height = maxY - minY;
                    }

                    bool isDup = false;
                    for (const auto& existing : results) {
                        if (isDuplicateQR(qr, existing)) { isDup = true; break; }
                    }
                    if (!isDup) results.push_back(qr);
                }
                symbol = zbar_symbol_next(symbol);
            }
        }
        zbar_image_destroy(image);
    }

    /**
     * 用一种预处理配置扫描，多尺度
     */
    void scanWithConfig(const cv::Mat& frame, const PreprocessConfig& cfg,
                        std::vector<QRResult>& results) {
        preprocessor_->setConfig(cfg);
        cv::Mat preprocessed;
        preprocessor_->process(frame, preprocessed);

        // 多尺度：1x, 1.5x, 2x, 2.5x, 3x
        for (double s = 1.0; s <= 3.01; s += 0.5) {
            scanAtScale(preprocessed, s, results);
            if (results.size() >= MAX_QR_COUNT) return;
        }
    }

    /**
     * 多策略处理帧（与 v1 完全一致的识别逻辑）
     */
    bool processFrame(cv::Mat& frame, cv::Mat& outFrame,
                      std::vector<QRResult>& results, double& processTime) {
        auto start = std::chrono::high_resolution_clock::now();
        if (frame.empty()) return false;

        results.clear();
        frame.copyTo(outFrame);

        // ★ 自动放大：如果图片太小（短边<300），先放大
        cv::Mat workFrame;
        int minDim = std::min(frame.cols, frame.rows);
        if (minDim < 300) {
            double upscale = 300.0 / minDim;
            cv::resize(frame, workFrame, cv::Size(), upscale, upscale, cv::INTER_LINEAR);
        } else {
            workFrame = frame;
        }

        // 策略1: 标准流程（灰度+模糊+二值化+CLAHE+锐化）
        {
            PreprocessConfig cfg;
            cfg.enableGrayConvert = true;
            cfg.enableGaussianBlur = true;
            cfg.enableAdaptiveBinary = true;
            cfg.enableMorphology = false;
            cfg.enableCLAHE = true;
            cfg.enableSharpen = true;
            scanWithConfig(workFrame, cfg, results);
            if (!results.empty()) goto done;
        }

        // 策略2: 无模糊（保留细节，适合小图/锐利图）
        {
            PreprocessConfig cfg;
            cfg.enableGrayConvert = true;
            cfg.enableGaussianBlur = false;
            cfg.enableAdaptiveBinary = true;
            cfg.enableMorphology = false;
            cfg.enableCLAHE = true;
            cfg.enableSharpen = true;
            scanWithConfig(workFrame, cfg, results);
            if (!results.empty()) goto done;
        }

        // 策略3: 最简（灰度+二值化，高对比度场景）
        {
            PreprocessConfig cfg;
            cfg.enableGrayConvert = true;
            cfg.enableGaussianBlur = false;
            cfg.enableAdaptiveBinary = true;
            cfg.enableMorphology = false;
            cfg.enableCLAHE = false;
            cfg.enableSharpen = false;
            scanWithConfig(workFrame, cfg, results);
            if (!results.empty()) goto done;
        }

        // 策略4: 灰度+模糊+二值化+形态学（修复断裂码）
        {
            PreprocessConfig cfg;
            cfg.enableGrayConvert = true;
            cfg.enableGaussianBlur = true;
            cfg.enableAdaptiveBinary = true;
            cfg.enableMorphology = true;
            cfg.morphologyKernelSize = 3;
            cfg.morphologyIterations = 1;
            cfg.enableCLAHE = false;
            cfg.enableSharpen = false;
            scanWithConfig(workFrame, cfg, results);
            if (!results.empty()) goto done;
        }

        // 策略5: 灰度直接扫（高质量图）
        {
            PreprocessConfig cfg;
            cfg.enableGrayConvert = true;
            cfg.enableGaussianBlur = false;
            cfg.enableAdaptiveBinary = false;
            cfg.enableMorphology = false;
            cfg.enableCLAHE = false;
            cfg.enableSharpen = false;
            scanWithConfig(workFrame, cfg, results);
        }

done:
        // 如果图片被放大了，坐标要缩放回去
        if (minDim < 300) {
            double downscale = (double)minDim / 300.0;
            for (auto& qr : results) {
                qr.x = (int)(qr.x * downscale);
                qr.y = (int)(qr.y * downscale);
                qr.width = (int)(qr.width * downscale);
                qr.height = (int)(qr.height * downscale);
            }
        }

        // 在原始尺寸输出图上画绿框 + 文本
        for (const auto& qr : results) {
            if (qr.width > 0 && qr.height > 0) {
                cv::rectangle(outFrame,
                              cv::Point(qr.x, qr.y),
                              cv::Point(qr.x + qr.width, qr.y + qr.height),
                              cv::Scalar(0, 255, 0), 2);
                cv::putText(outFrame,
                            qr.text.substr(0, 30),
                            cv::Point(qr.x, qr.y - 5),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5,
                            cv::Scalar(0, 255, 0), 1);
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        processTime = std::chrono::duration<double, std::milli>(end - start).count();
        return true;
    }

    /**
     * 快速处理帧（摄像头模式，仅灰度+单尺度扫描）
     */
    bool processFrameFast(cv::Mat& frame, cv::Mat& outFrame,
                          std::vector<QRResult>& results, double& processTime) {
        auto start = std::chrono::high_resolution_clock::now();
        if (frame.empty()) return false;

        results.clear();
        frame.copyTo(outFrame);

        // 仅灰度转换，不做任何预处理
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

        // 只在原始尺度扫描，不做多尺度
        scanAtScale(gray, 1.0, results);

        // 画绿框 + 文本
        for (const auto& qr : results) {
            if (qr.width > 0 && qr.height > 0) {
                cv::rectangle(outFrame,
                              cv::Point(qr.x, qr.y),
                              cv::Point(qr.x + qr.width, qr.y + qr.height),
                              cv::Scalar(0, 255, 0), 2);
                cv::putText(outFrame,
                            qr.text.substr(0, 30),
                            cv::Point(qr.x, qr.y - 5),
                            cv::FONT_HERSHEY_SIMPLEX, 0.5,
                            cv::Scalar(0, 255, 0), 1);
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        processTime = std::chrono::duration<double, std::milli>(end - start).count();
        return true;
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_;
    zbar_image_scanner_t* scanner_;
    ImagePreprocessor* preprocessor_;
};

/* ===== 全局状态 ===== */
static QRDetectorState* g_state = nullptr;
static std::mutex g_mutex;

/* ===== 上次处理的结果缓存（供 GetQRText / GetQRRect 读取） ===== */
static std::vector<QRResult> g_lastResults;

/* ===== 导出函数 ===== */

int InitDetector() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state) {
        g_state->release();
        delete g_state;
    }
    g_state = new QRDetectorState();
    if (!g_state->initialize()) {
        delete g_state;
        g_state = nullptr;
        return QR_ERR_INIT;
    }
    g_lastResults.clear();
    return QR_OK;
}

int ProcessImageFile(const char* inputPath) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_state || !g_state->initialized()) return QR_ERR_INIT;
    if (!inputPath) return QR_ERR_PROCESS;

    cv::Mat frame = cv::imread(inputPath);
    if (frame.empty()) return QR_ERR_PROCESS;

    cv::Mat outFrame;
    std::vector<QRResult> results;
    double procTime;
    g_state->processFrame(frame, outFrame, results, procTime);

    // 保存标注图到固定路径
    std::string annotatedPath = getAnnotatedPath();
    cv::imwrite(annotatedPath, outFrame);

    // 缓存结果
    g_lastResults = results;

    return (int)results.size();
}

int ProcessImageData(
    unsigned char* imageData,
    int width,
    int height,
    int colorOrder)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_state || !g_state->initialized()) return QR_ERR_INIT;
    if (!imageData || width <= 0 || height <= 0) return QR_ERR_PROCESS;

    cv::Mat frame;
    if (colorOrder == 2) {
            // U32 输入（LabVIEW IMAQ ColorImageToArray 2D数组，CLF配置Dimensions=2）
            // LabVIEW 2D数组是列优先(column-major)，内存：先第0列所有行，再第1列...
            // 用 Mat(width, height, ...) 按行读取：每行是一列数据
            // transpose 后得到 height×width 行优先布局
            cv::Mat raw_colmajor(width, height, CV_8UC4, imageData);
            cv::Mat raw;
            cv::transpose(raw_colmajor, raw);
            // transpose 后方向偏了，再顺时针旋转90度修正
            cv::rotate(raw, raw, cv::ROTATE_90_CLOCKWISE);
            // raw 现在是 height×width，方向正确

            // IMAQ U32 像素格式: 0x00RRGGBB (小端内存: BB GG RR 00 = BGRA)
            cv::cvtColor(raw, frame, cv::COLOR_BGRA2BGR);
    } else if (colorOrder == 1) {
        // RGB 3字节/像素（IMAQdx 默认输出）
        cv::Mat raw(height, width, CV_8UC3, imageData);
        cv::cvtColor(raw, frame, cv::COLOR_RGB2BGR);
    } else {
        // BGR 3字节/像素（默认）
        frame = cv::Mat(height, width, CV_8UC3, imageData).clone();
    }

    cv::Mat outFrame;
    std::vector<QRResult> results;
    double procTime;
    // 摄像头实时数据走快速路径（仅灰度+2尺度），避免多策略多尺度导致卡顿
    g_state->processFrameFast(frame, outFrame, results, procTime);

    // 保存标注图（BMP格式，无压缩，写入极快）
    std::string annotatedPath = getAnnotatedPath();
    cv::imwrite(annotatedPath, outFrame);

    // 缓存结果
    g_lastResults = results;

    return (int)results.size();
}

int GetQRText(int index, char* buffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!buffer || bufferSize <= 0) return QR_ERR_PROCESS;
    if (index < 0 || index >= (int)g_lastResults.size()) return QR_ERR_INDEX;

    const std::string& text = g_lastResults[index].text;
    int copyLen = (int)text.size() < bufferSize - 1 ? (int)text.size() : bufferSize - 1;
    memcpy(buffer, text.c_str(), copyLen);
    buffer[copyLen] = '\0';

    return QR_OK;
}

int GetQRRect(int index, int* outX, int* outY, int* outW, int* outH) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (index < 0 || index >= (int)g_lastResults.size()) return QR_ERR_INDEX;

    if (outX) *outX = g_lastResults[index].x;
    if (outY) *outY = g_lastResults[index].y;
    if (outW) *outW = g_lastResults[index].width;
    if (outH) *outH = g_lastResults[index].height;

    return QR_OK;
}

int GetAnnotatedImagePath(char* buffer, int bufferSize) {
    if (!buffer || bufferSize <= 0) return QR_ERR_PROCESS;

    std::string path = getAnnotatedPath();
    int copyLen = (int)path.size() < bufferSize - 1 ? (int)path.size() : bufferSize - 1;
    memcpy(buffer, path.c_str(), copyLen);
    buffer[copyLen] = '\0';

    return QR_OK;
}

void ReleaseDetector() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state) {
        g_state->release();
        delete g_state;
        g_state = nullptr;
    }
    g_lastResults.clear();
}

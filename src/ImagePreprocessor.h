/**
 * ImagePreprocessor.h - 图像预处理模块
 * 
 * 提供二维码识别前的图像预处理功能：
 *   - 灰度化
 *   - CLAHE 对比度增强（解决远距离/倾斜场景对比度不足）
 *   - 锐化增强（解决模糊/远距离场景边缘不清）
 *   - 高斯滤波去噪
 *   - 自适应二值化
 *   - 形态学处理（开运算/闭运算）
 * 
 * 所有参数可通过外部配置，支持灵活的预处理流水线组合。
 */

#ifndef IMAGE_PREPROCESSOR_H
#define IMAGE_PREPROCESSOR_H

#include <opencv2/opencv.hpp>

/**
 * 预处理参数配置结构体
 * LabVIEW 前端可实时修改这些参数
 */
struct PreprocessConfig {
    bool enableGrayConvert;      // 启用灰度化（默认 true）
    bool enableCLAHE;            // 启用 CLAHE 对比度增强（默认 false）
    bool enableSharpen;          // 启用锐化增强（默认 false）
    bool enableGaussianBlur;     // 启用高斯滤波（默认 true）
    bool enableAdaptiveBinary;   // 启用自适应二值化（默认 true）
    bool enableMorphology;       // 启用形态学处理（默认 false）

    double claheClipLimit;       // CLAHE 裁剪限制（默认 2.0，值越大对比度增强越强）
    int claheGridSize;           // CLAHE 网格大小（默认 8，必须为正整数）

    int gaussianKernelSize;      // 高斯滤波核大小（必须为奇数，默认 5）
    double gaussianSigma;        // 高斯滤波 sigma（默认 0 = 自动计算）

    int adaptiveBlockSize;       // 自适应二值化邻域块大小（必须为奇数，默认 11）
    int adaptiveC;               // 自适应二值化常数偏移（默认 2）

    int morphologyKernelSize;    // 形态学核大小（默认 3）
    int morphologyIterations;    // 形态学迭代次数（默认 1）

    PreprocessConfig()
        : enableGrayConvert(true)
        , enableCLAHE(false)
        , enableSharpen(false)
        , enableGaussianBlur(true)
        , enableAdaptiveBinary(true)
        , enableMorphology(false)
        , claheClipLimit(2.0)
        , claheGridSize(8)
        , gaussianKernelSize(5)
        , gaussianSigma(0)
        , adaptiveBlockSize(11)
        , adaptiveC(2)
        , morphologyKernelSize(3)
        , morphologyIterations(1)
    {}
};

class ImagePreprocessor {
public:
    ImagePreprocessor();
    ~ImagePreprocessor();

    /**
     * 设置预处理参数
     */
    void setConfig(const PreprocessConfig& config);

    /**
     * 获取当前预处理参数
     */
    PreprocessConfig getConfig() const;

    /**
     * 执行完整的预处理流水线
     * @param input  输入图像（BGR 格式）
     * @param output 输出图像（预处理后的结果）
     * @return 处理耗时（毫秒）
     */
    double process(const cv::Mat& input, cv::Mat& output);

    /**
     * 执行单步预处理（用于 LabVIEW 独立控制每个步骤）
     */
    cv::Mat toGray(const cv::Mat& input);
    cv::Mat applyCLAHE(const cv::Mat& input);
    cv::Mat applySharpen(const cv::Mat& input);
    cv::Mat applyGaussianBlur(const cv::Mat& input);
    cv::Mat applyAdaptiveBinary(const cv::Mat& input);
    cv::Mat applyMorphology(const cv::Mat& input);

    /**
     * 获取上一次处理时每个步骤的中间图像（用于调试/显示）
     */
    const cv::Mat& getGrayImage() const;
    const cv::Mat& getCLAHEImage() const;
    const cv::Mat& getSharpenImage() const;
    const cv::Mat& getBlurredImage() const;
    const cv::Mat& getBinaryImage() const;
    const cv::Mat& getMorphImage() const;

private:
    PreprocessConfig config_;
    cv::Mat grayImg_;
    cv::Mat claheImg_;
    cv::Mat sharpenImg_;
    cv::Mat blurredImg_;
    cv::Mat binaryImg_;
    cv::Mat morphImg_;
};

#endif // IMAGE_PREPROCESSOR_H

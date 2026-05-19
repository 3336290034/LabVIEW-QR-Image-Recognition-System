/**
 * ImagePreprocessor.cpp - 图像预处理模块实现
 */

#include "ImagePreprocessor.h"
#include <chrono>

ImagePreprocessor::ImagePreprocessor() {}

ImagePreprocessor::~ImagePreprocessor() {}

void ImagePreprocessor::setConfig(const PreprocessConfig& config) {
    config_ = config;
}

PreprocessConfig ImagePreprocessor::getConfig() const {
    return config_;
}

double ImagePreprocessor::process(const cv::Mat& input, cv::Mat& output) {
    auto startTime = std::chrono::high_resolution_clock::now();

    cv::Mat current = input.clone();

    // Step 1: 灰度化（ZBar 需要）
    if (config_.enableGrayConvert && current.channels() > 1) {
        current = toGray(current);
    }

    // Step 2: CLAHE 对比度增强（解决远距离/倾斜场景对比度不足）
    if (config_.enableCLAHE) {
        current = applyCLAHE(current);
    }

    // Step 3: 锐化增强（解决模糊/远距离场景边缘不清）
    if (config_.enableSharpen) {
        current = applySharpen(current);
    }

    // Step 4: 高斯滤波
    if (config_.enableGaussianBlur) {
        current = applyGaussianBlur(current);
    }

    // Step 5: 自适应二值化
    if (config_.enableAdaptiveBinary) {
        current = applyAdaptiveBinary(current);
    }

    // Step 6: 形态学处理
    if (config_.enableMorphology) {
        current = applyMorphology(current);
    }

    output = current;

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = endTime - startTime;
    return elapsed.count();
}

cv::Mat ImagePreprocessor::toGray(const cv::Mat& input) {
    cv::Mat gray;
    if (input.channels() == 3) {
        cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
    } else if (input.channels() == 4) {
        cv::cvtColor(input, gray, cv::COLOR_BGRA2GRAY);
    } else {
        gray = input.clone();
    }
    grayImg_ = gray;
    return gray;
}

cv::Mat ImagePreprocessor::applyCLAHE(const cv::Mat& input) {
    cv::Mat enhanced;
    double clipLimit = config_.claheClipLimit;
    if (clipLimit <= 0) clipLimit = 2.0;

    int gridSize = config_.claheGridSize;
    if (gridSize <= 0) gridSize = 8;

    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit, cv::Size(gridSize, gridSize));
    clahe->apply(input, enhanced);
    claheImg_ = enhanced;
    return enhanced;
}

cv::Mat ImagePreprocessor::applySharpen(const cv::Mat& input) {
    cv::Mat sharpened;
    // Laplacian 锐化核：增强边缘，帮助远距离/模糊二维码的定位
    cv::Mat kernel = (cv::Mat_<float>(3, 3) <<
        0, -1, 0,
       -1,  5, -1,
        0, -1, 0);
    cv::filter2D(input, sharpened, -1, kernel);
    sharpenImg_ = sharpened;
    return sharpened;
}

cv::Mat ImagePreprocessor::applyGaussianBlur(const cv::Mat& input) {
    cv::Mat blurred;
    int ksize = config_.gaussianKernelSize;
    // 确保核大小为奇数
    if (ksize % 2 == 0) ksize++;
    if (ksize < 3) ksize = 3;

    cv::GaussianBlur(input, blurred, cv::Size(ksize, ksize), config_.gaussianSigma);
    blurredImg_ = blurred;
    return blurred;
}

cv::Mat ImagePreprocessor::applyAdaptiveBinary(const cv::Mat& input) {
    cv::Mat binary;
    int blockSize = config_.adaptiveBlockSize;
    // 确保块大小为奇数
    if (blockSize % 2 == 0) blockSize++;
    if (blockSize < 3) blockSize = 3;

    cv::adaptiveThreshold(
        input, binary, 255,
        cv::ADAPTIVE_THRESH_GAUSSIAN_C,
        cv::THRESH_BINARY,
        blockSize,
        config_.adaptiveC
    );
    binaryImg_ = binary;
    return binary;
}

cv::Mat ImagePreprocessor::applyMorphology(const cv::Mat& input) {
    cv::Mat morphed;
    int ksize = config_.morphologyKernelSize;
    if (ksize % 2 == 0) ksize++;
    if (ksize < 3) ksize = 3;

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(ksize, ksize));

    // 先开运算去除噪点，再闭运算填充空洞
    cv::morphologyEx(input, morphed, cv::MORPH_OPEN, kernel,
                     cv::Point(-1, -1), config_.morphologyIterations);
    cv::morphologyEx(morphed, morphed, cv::MORPH_CLOSE, kernel,
                     cv::Point(-1, -1), config_.morphologyIterations);

    morphImg_ = morphed;
    return morphed;
}

const cv::Mat& ImagePreprocessor::getGrayImage() const { return grayImg_; }
const cv::Mat& ImagePreprocessor::getCLAHEImage() const { return claheImg_; }
const cv::Mat& ImagePreprocessor::getSharpenImage() const { return sharpenImg_; }
const cv::Mat& ImagePreprocessor::getBlurredImage() const { return blurredImg_; }
const cv::Mat& ImagePreprocessor::getBinaryImage() const { return binaryImg_; }
const cv::Mat& ImagePreprocessor::getMorphImage() const { return morphImg_; }

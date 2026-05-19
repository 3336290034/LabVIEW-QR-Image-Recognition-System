/**
* test_decoder.cpp - 测试程序 (v2 - 适配简化API)
*
* 新 API: InitDetector / ProcessImageFile / ProcessImageData
*         GetQRText / GetQRRect / GetAnnotatedImagePath / ReleaseDetector
*/

#include "QRDecoder.h"
#include <opencv2/opencv.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

/* ===== 全局配置 ===== */
static bool g_display = false;
static bool g_enhance = false;   // --enhance: 启用多策略+多尺度增强（否则走快速路径）
static int  g_cameraIndex = 0;
static const char* WINDOW_NAME = "QR Decoder";
static const int WIN_W = 800;
static const int WIN_H = 600;

/* ===== 辅助：自适应显示图像 ===== */
static void showFit(const cv::Mat& img, const char* winName = WINDOW_NAME) {
    cv::namedWindow(winName, cv::WINDOW_NORMAL);
    cv::resizeWindow(winName, WIN_W, WIN_H);
    cv::imshow(winName, img);
}

/* ===== 测试单张图片 ===== */
static int testImage(const char* imagePath) {
    int qrCount;

    if (g_enhance) {
        // 增强模式：走 ProcessImageFile → processFrame（5策略×5尺度）
        qrCount = ProcessImageFile(imagePath);
    } else {
        // 快速模式：走 ProcessImageData → processFrameFast（灰度+1x/2x）
        cv::Mat frame = cv::imread(imagePath);
        if (frame.empty()) return -3;
        qrCount = ProcessImageData(frame.data, frame.cols, frame.rows, 0);
    }

    if (qrCount < 0) {
        printf("  Error: %d\n", qrCount);
        return qrCount;
    }

    printf("%d QR codes found", qrCount);
    for (int i = 0; i < qrCount; i++) {
        char text[2048] = {0};
        int x = 0, y = 0, w = 0, h = 0;
        GetQRText(i, text, sizeof(text));
        GetQRRect(i, &x, &y, &w, &h);
        printf("\n  QR#%d: \"%s\" (%d,%d) %dx%d", i + 1, text, x, y, w, h);
    }
    printf("\n");

    // 显示标注图
    if (g_display) {
        char imgPath[512] = {0};
        GetAnnotatedImagePath(imgPath, sizeof(imgPath));
        cv::Mat img = cv::imread(imgPath);
        if (!img.empty()) {
            showFit(img);
            int key = cv::waitKey(0);
            if (key == 'q') return -999;
        }
    }

    return qrCount > 0 ? 1 : 0;
}

/* ===== 测试批量目录 ===== */
static int testDirectory(const char* dirPath) {
    std::vector<std::string> imageFiles;
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tif" || ext == ".tiff") {
            imageFiles.push_back(entry.path().string());
        }
    }
    std::sort(imageFiles.begin(), imageFiles.end());

    if (imageFiles.empty()) {
        printf("No images found in: %s\n", dirPath);
        return 0;
    }

    printf("Found %zu images\n", imageFiles.size());
    printf("Mode: %s\n\n", g_enhance ? "ENHANCED (5-strategy x 5-scale)" : "FAST (gray + 1x/2x)");

    int success = 0, fail = 0, error = 0;
    int earlyExit = 0;
    auto tTotalStart = std::chrono::steady_clock::now();

    for (size_t idx = 0; idx < imageFiles.size(); idx++) {
        printf("[%zu/%zu] %s: ", idx + 1, imageFiles.size(),
               fs::path(imageFiles[idx]).filename().string().c_str());

        auto tImgStart = std::chrono::steady_clock::now();
        int result = testImage(imageFiles[idx].c_str());
        auto tImgEnd = std::chrono::steady_clock::now();
        double imgMs = std::chrono::duration<double, std::milli>(tImgEnd - tImgStart).count();
        printf("  [%.1f ms]\n", imgMs);

        if (result == -999) { earlyExit = 1; break; }
        if (result < 0) { error++; }
        else if (result > 0) { success++; }
        else { fail++; }
    }

    auto tTotalEnd = std::chrono::steady_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(tTotalEnd - tTotalStart).count();

    printf("\n============================================\n");
    printf("Total: %zu  Success: %d  Fail: %d  Error: %d\n",
           imageFiles.size(), success, fail, error);
    if (imageFiles.size() > 0) {
        printf("Detection rate: %.1f%%\n", 100.0 * success / imageFiles.size());
        printf("Total time: %.1f ms  Avg: %.1f ms/image\n", totalMs, totalMs / imageFiles.size());
    }
    printf("============================================\n");

    return success;
}

/* ===== 测试摄像头 ===== */
static int testCamera(int cameraIndex) {
    cv::VideoCapture cap(cameraIndex);
    if (!cap.isOpened()) {
        printf("Cannot open camera %d\n", cameraIndex);
        return -1;
    }

    printf("Camera %d opened. Press 'q' to quit, 's' to screenshot.\n", cameraIndex);

    cv::Mat frame;
    int frameCount = 0;
    double fps = 0.0;
    auto tStart = std::chrono::steady_clock::now();
    auto tFps = tStart;

    while (true) {
        cap >> frame;
        if (frame.empty()) break;

        frameCount++;

        int qrCount = ProcessImageData(frame.data, frame.cols, frame.rows, 0);

        if (qrCount > 0) {
            for (int i = 0; i < qrCount; i++) {
                char text[2048] = {0};
                int x = 0, y = 0, w = 0, h = 0;
                GetQRText(i, text, sizeof(text));
                GetQRRect(i, &x, &y, &w, &h);
                printf("QR#%d: \"%s\" (%d,%d) %dx%d\n", i + 1, text, x, y, w, h);
            }
        }

        // 计算帧率
        auto tNow = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(tNow - tFps).count();
        if (elapsed >= 1.0) {
            fps = frameCount / std::chrono::duration<double>(tNow - tStart).count();
            printf("[FPS: %.1f]\n", fps);
            tFps = tNow;
        }

        // 在画面上叠加帧率文字
        cv::Mat display = frame.clone();
        std::string fpsText = "FPS: " + std::to_string((int)fps);
        cv::putText(display, fpsText, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);

        if (g_display) {
            char imgPath[512] = {0};
            GetAnnotatedImagePath(imgPath, sizeof(imgPath));
            cv::Mat annotated = cv::imread(imgPath);
            if (!annotated.empty()) {
                cv::putText(annotated, fpsText, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);
                showFit(annotated, "QR Camera");
            }
        } else {
            showFit(display, "QR Camera");
        }

        int key = cv::waitKey(30);
        if (key == 'q') break;
        if (key == 's') {
            std::string filename = "screenshot_" + std::to_string(std::time(nullptr)) + ".bmp";
            cv::imwrite(filename, frame);
            printf("Screenshot saved: %s\n", filename.c_str());
        }
    }

    cap.release();
    cv::destroyAllWindows();
    return 0;
}

/* ===== 测试视频 ===== */
static int testVideo(const char* videoPath) {
    cv::VideoCapture cap(videoPath);
    if (!cap.isOpened()) {
        printf("Cannot open video: %s\n", videoPath);
        return -1;
    }

    int totalFrames = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
    double fps = cap.get(cv::CAP_PROP_FPS);
    printf("Video: %d frames, %.1f fps\n\n", totalFrames, fps);

    cv::Mat frame;
    int successCount = 0;
    int frameIdx = 0;

    while (cap.read(frame)) {
        frameIdx++;

        int qrCount = ProcessImageData(frame.data, frame.cols, frame.rows, 0);

        if (qrCount > 0) successCount++;
        printf("\rFrame %d/%d  QR:%d", frameIdx, totalFrames, qrCount);

        if (g_display && frameIdx % 3 == 0) {
            char imgPath[512] = {0};
            GetAnnotatedImagePath(imgPath, sizeof(imgPath));
            cv::Mat annotated = cv::imread(imgPath);
            if (!annotated.empty()) {
                showFit(annotated, "QR Video");
                if (cv::waitKey(1) == 'q') break;
            }
        }
    }

    printf("\n\nResult: %d/%d frames detected (%.1f%%)\n",
           successCount, frameIdx, 100.0 * successCount / std::max(frameIdx, 1));

    cap.release();
    cv::destroyAllWindows();
    return 0;
}

/* ===== 主函数 ===== */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("QRDecoder Test (v2 Simplified API)\n\n");
        printf("Usage:\n");
        printf("  test_decoder.exe --dir <directory>     Batch test images\n");
        printf("  test_decoder.exe --image <image_path>  Test single image\n");
        printf("  test_decoder.exe --camera [index]       Webcam mode\n");
        printf("  test_decoder.exe <video_path>           Test video\n\n");
        printf("Options:\n");
        printf("  --display       Show visual window\n");
        printf("  --enhance       Enable multi-strategy + multi-scale (5x5)\n");
        printf("                  Default: fast mode (gray + 1x/2x only)\n");
        return 0;
    }

    // 解析参数
    std::string mode, path;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--display") { g_display = true; }
        else if (arg == "--enhance") { g_enhance = true; }
        else if (arg == "--dir" && i + 1 < argc) { mode = "dir"; path = argv[++i]; }
        else if (arg == "--image" && i + 1 < argc) { mode = "image"; path = argv[++i]; }
        else if (arg == "--camera") {
            mode = "camera";
            g_cameraIndex = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : 0;
        }
        else if (arg[0] != '-') { mode = "video"; path = arg; }
    }

    // 初始化
    int ret = InitDetector();
    if (ret != QR_OK) {
        printf("InitDetector failed: %d\n", ret);
        return 1;
    }

    printf("InitDetector OK\n");

    // 运行测试
    if (mode == "image") {
        printf("\n=== Image Test ===\n\n");
        testImage(path.c_str());
    } else if (mode == "dir") {
        printf("\n=== Directory Test ===\n\n");
        testDirectory(path.c_str());
    } else if (mode == "camera") {
        printf("\n=== Camera Test ===\n\n");
        testCamera(g_cameraIndex);
    } else if (mode == "video") {
        printf("\n=== Video Test ===\n\n");
        testVideo(path.c_str());
    }

    ReleaseDetector();
    printf("\nDone.\n");
    return 0;
}

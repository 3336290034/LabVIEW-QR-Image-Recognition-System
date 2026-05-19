@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   QRDecoder Build Script (x64 MinGW)
echo   OpenCV 4.14.0 + ZBar 0.23.93 (Static)
echo ============================================
echo.

:: ===== 路径配置 =====
set "PROJECT_DIR=D:\Code\OpenCV\qr_decoder_project"
set "OPENCV_DIR=D:\Code\OpenCV\install"
set "ZBAR_DIR=%PROJECT_DIR%\third_party\zbar"
set "OUTPUT_DIR=%PROJECT_DIR%\output"

set "OPENCV_INCLUDE=%OPENCV_DIR%\include"
set "OPENCV_LIB=%OPENCV_DIR%\x64\mingw\lib"
set "OPENCV_DLL=%OPENCV_DIR%\x64\mingw\bin"

set "ZBAR_INCLUDE=%ZBAR_DIR%\include"
set "ZBAR_LIB=%ZBAR_DIR%\lib"

set "SRC_DIR=%PROJECT_DIR%\src"
set "TEST_DIR=%PROJECT_DIR%\test"

:: ===== 创建输出目录 =====
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

:: ===== 编译器检查 =====
where g++ >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] g++ not found! Please add MinGW bin to PATH.
    exit /b 1
)

:: ===== [1/4] 编译 clock_compat.c 兼容层 =====
echo [1/4] Compiling clock_compat.c ...
echo.

gcc -c "%SRC_DIR%\clock_compat.c" -o "%OUTPUT_DIR%\clock_compat.o"

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] clock_compat.c compilation FAILED!
    exit /b 1
)

:: ===== [2/4] 编译 QRDecoder.dll（静态链接 ZBar） =====
echo.
echo [2/4] Building QRDecoder.dll (static ZBar) ...
echo.

g++ -shared -o "%OUTPUT_DIR%\QRDecoder.dll" ^
    "%SRC_DIR%\QRDecoder.cpp" ^
    "%SRC_DIR%\ImagePreprocessor.cpp" ^
    "%OUTPUT_DIR%\clock_compat.o" ^
    -I"%SRC_DIR%" ^
    -I"%OPENCV_INCLUDE%" ^
    -I"%ZBAR_INCLUDE%" ^
    -L"%OPENCV_LIB%" ^
    "%ZBAR_LIB%\libzbar.a" ^
    -lopencv_core4140 ^
    -lopencv_imgproc4140 ^
    -lopencv_imgcodecs4140 ^
    -lopencv_videoio4140 ^
    -Wl,--out-implib,"%OUTPUT_DIR%\QRDecoder.lib" ^
    -O2 -DNDEBUG -fpermissive -Wno-deprecated-declarations ^
    -std=c++17

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] DLL build FAILED!
    exit /b 1
)

:: ===== [3/4] 编译测试程序（始终链接 highgui，运行时 --display 控制） =====
echo.
echo [3/4] Building test_decoder.exe ...
echo.

g++ -o "%OUTPUT_DIR%\test_decoder.exe" ^
    "%TEST_DIR%\test_decoder.cpp" ^
    "%OUTPUT_DIR%\clock_compat.o" ^
    -I"%SRC_DIR%" ^
    -I"%OPENCV_INCLUDE%" ^
    -I"%ZBAR_INCLUDE%" ^
    -L"%OUTPUT_DIR%" ^
    -L"%OPENCV_LIB%" ^
    -lQRDecoder ^
    "%ZBAR_LIB%\libzbar.a" ^
    -lopencv_core4140 -lopencv_imgproc4140 -lopencv_imgcodecs4140 ^
    -lopencv_videoio4140 -lopencv_highgui4140 ^
    -O2 -DNDEBUG -fpermissive -Wno-deprecated-declarations ^
    -std=c++17 -DUSE_OPENCV_DISPLAY

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Test program build FAILED!
    exit /b 1
)

:: ===== [4/4] 复制运行时 DLL =====
echo.
echo [4/4] Copying runtime DLLs ...
echo.

copy /Y "%OPENCV_DLL%\libopencv_core4140.dll" "%OUTPUT_DIR%\" >nul 2>&1
copy /Y "%OPENCV_DLL%\libopencv_imgproc4140.dll" "%OUTPUT_DIR%\" >nul 2>&1
copy /Y "%OPENCV_DLL%\libopencv_imgcodecs4140.dll" "%OUTPUT_DIR%\" >nul 2>&1
copy /Y "%OPENCV_DLL%\libopencv_videoio4140.dll" "%OUTPUT_DIR%\" >nul 2>&1
copy /Y "%OPENCV_DLL%\libopencv_highgui4140.dll" "%OUTPUT_DIR%\" >nul 2>&1
copy /Y "%OPENCV_DLL%\opencv_videoio_ffmpeg4140_64.dll" "%OUTPUT_DIR%\" >nul 2>&1

:: 注意：不再复制 libzbar-0.dll，ZBar 已静态链接进 QRDecoder.dll
:: 注意：libwinpthread-1.dll 必须来自 MinGW 版本（D:\MinGW\mingw64\bin），不能使用 MSYS2 版本

echo.
echo ============================================
echo   BUILD SUCCESS!
echo ============================================
echo.
echo Output: %OUTPUT_DIR%\test_decoder.exe
echo.
echo Usage:
echo   test_decoder.exe --dir ^<directory^>     Batch test images
echo   test_decoder.exe --image ^<image_path^>  Test single image
echo   test_decoder.exe --camera [index]        Webcam mode
echo   test_decoder.exe ^<video_path^>          Test video
echo.
echo Options:
echo   --display       Show visual window (per image popup)
echo   --enhance       CLAHE + Sharpen (far/tilted/blurry)
echo   --multiscale N  Multi-scale scan (N=max scale, 2~5)
echo.

:: 清理临时编译产物
del /Q "%OUTPUT_DIR%\clock_compat.o" >nul 2>&1

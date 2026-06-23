@echo off
setlocal

:: --- CONFIGURATION START ---

if defined ANDROID_SDK_ROOT (
    set "SDK_ROOT=%ANDROID_SDK_ROOT%"
) else if defined ANDROID_HOME (
    set "SDK_ROOT=%ANDROID_HOME%"
) else (
    set "SDK_ROOT="
)

:: 1. NDK Path
:: Try to find NDK from environment variable first
if defined ANDROID_NDK_HOME (
    set "NDK_ROOT=%ANDROID_NDK_HOME%"
) else (
    if "%SDK_ROOT%"=="" (
        echo [ERROR] ANDROID_SDK_ROOT/ANDROID_HOME 未设置，无法自动定位 NDK。
        echo 请设置 ANDROID_NDK_HOME，或设置 ANDROID_SDK_ROOT/ANDROID_HOME 指向 Android SDK 根目录。
        exit /b 1
    )
    if exist "%SDK_ROOT%\ndk\27.0.12077973" (
        set "NDK_ROOT=%SDK_ROOT%\ndk\27.0.12077973"
    ) else if exist "%SDK_ROOT%\ndk\25.1.8937393" (
        set "NDK_ROOT=%SDK_ROOT%\ndk\25.1.8937393"
    )
)

:: 2. CMake (from AndroidStudioSDK)
set "CMAKE_EXE="
if exist "%SDK_ROOT%\cmake\4.1.2\bin\cmake.exe" set "CMAKE_EXE=%SDK_ROOT%\cmake\4.1.2\bin\cmake.exe"
if "%CMAKE_EXE%"=="" if exist "%SDK_ROOT%\cmake\3.22.1\bin\cmake.exe" set "CMAKE_EXE=%SDK_ROOT%\cmake\3.22.1\bin\cmake.exe"
if "%CMAKE_EXE%"=="" set "CMAKE_EXE=cmake"
set "NINJA_EXE="
if exist "%SDK_ROOT%\cmake\4.1.2\bin\ninja.exe" set "NINJA_EXE=%SDK_ROOT%\cmake\4.1.2\bin\ninja.exe"
if "%NINJA_EXE%"=="" if exist "%SDK_ROOT%\cmake\3.22.1\bin\ninja.exe" set "NINJA_EXE=%SDK_ROOT%\cmake\3.22.1\bin\ninja.exe"

:: 3. OpenCV Path (from environment variable, avoid hardcoding)
if defined OPENCV_ROOT (
    set "OPENCV_ROOT_PATH=%OPENCV_ROOT%"
) else (
    echo [ERROR] OPENCV_ROOT 未设置，无法定位 OpenCV 源码目录。
    echo 请设置 OPENCV_ROOT（例：D:\ProgramData\OpenCV\opencv-4.10.0）。
    exit /b 1
)
if defined OPENCV_CONTRIB_ROOT (
    set "OPENCV_CONTRIB_ROOT_PATH=%OPENCV_CONTRIB_ROOT%"
) else (
    echo [ERROR] OPENCV_CONTRIB_ROOT 未设置，无法定位 OpenCV Contrib 源码目录。
    echo 请设置 OPENCV_CONTRIB_ROOT（例：D:\ProgramData\OpenCV\opencv_contrib-4.10.0）。
    exit /b 1
)

:: --- CONFIGURATION END ---

if not exist "%NDK_ROOT%" (
    echo [ERROR] NDK not found at: %NDK_ROOT%
    echo Please set ANDROID_NDK_HOME or install NDK under %SDK_ROOT%\ndk.
    exit /b 1
)

:: Build Directory
set "BUILD_DIR=build_android_rk3288"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

set "PYTHONDONTWRITEBYTECODE=1"
set "PYTHONNOUSERSITE=1"
set "PYTHONPYCACHEPREFIX=%CD%\pycache"

echo ==============================================
echo  Building for RK3288 (ARM Cortex-A17)
echo  ABI: armeabi-v7a (32-bit)
echo  STL: c++_static
echo ==============================================

:: Configure CMake
:: Note: We use "NMake Makefiles" or default generator. If you have Ninja installed, add -G "Ninja"
"%CMAKE_EXE%" -G "Ninja" -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
      -DCMAKE_TOOLCHAIN_FILE="%NDK_ROOT%\build\cmake\android.toolchain.cmake" ^
      -DANDROID_ABI=armeabi-v7a ^
      -DANDROID_PLATFORM=android-21 ^
      -DANDROID_STL=c++_static ^
      -DOPENCV_ROOT="%OPENCV_ROOT_PATH%" ^
      -DOPENCV_CONTRIB_ROOT="%OPENCV_CONTRIB_ROOT_PATH%" ^
      -DWITH_IPP=OFF ^
      -DOPENCV_DOWNLOAD_IPPICV=OFF ^
      -DWITH_FFMPEG=OFF ^
      -DBUILD_LIST=core,imgproc,imgcodecs,objdetect,features2d ^
      -DCMAKE_BUILD_TYPE=Release ^
      .. > build_config.log 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configuration failed.
    type build_config.log
    exit /b 1
)

:: Build
echo.
echo Starting Build...
"%CMAKE_EXE%" --build . --config Release --target opencv_verify_cli -- -j4 > build.log 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    type build.log
    exit /b 1
)

echo.
echo [SUCCESS] Build complete!
echo Executable is located at: %CD%\opencv_verify_cli
echo.
echo To deploy to device:
echo   adb push %CD%\opencv_verify_cli /data/local/tmp/opencv_verify_cli
echo   adb shell "chmod +x /data/local/tmp/opencv_verify_cli"
echo   adb shell "/data/local/tmp/opencv_verify_cli --input /data/local/tmp/tests --cascade /data/local/tmp/lbpcascade_frontalface.xml"
echo.

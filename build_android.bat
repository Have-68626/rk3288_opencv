@echo off
setlocal

:: --- CONFIGURATION START ---

:: 1. NDK Path
:: Attempting to use the path found in your previous configuration
set "NDK_ROOT=D:\Program Files (x86)\Microsoft Visual Studio\Shared\Android\AndroidNDK\android-ndk-r23c"

:: 2. OpenCV Android SDK Path
:: IMPORTANT: You must download OpenCV Android SDK (e.g., 4.10.0) and extract it.
:: Set the path to the 'sdk/native/jni' folder below.
set "OPENCV_DIR=D:\OpenCV-android-sdk\sdk\native\jni"

:: --- CONFIGURATION END ---

if not exist "%NDK_ROOT%" (
    echo [ERROR] NDK not found at: %NDK_ROOT%
    echo Please edit 'build_android.bat' and set the correct NDK_ROOT.
    pause
    exit /b 1
)

if not exist "%OPENCV_DIR%" (
    echo [WARNING] OpenCV not found at: %OPENCV_DIR%
    echo Please edit 'build_android.bat' and set the correct OPENCV_DIR.
    echo The build will likely fail during 'find_package(OpenCV)'.
    pause
)

:: Build Directory
set "BUILD_DIR=build_android_rk3288"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

echo ==============================================
echo  Building for RK3288 (ARM Cortex-A17)
echo  ABI: armeabi-v7a (32-bit)
echo  STL: c++_static
echo ==============================================

:: Configure CMake
:: Note: We use "NMake Makefiles" or default generator. If you have Ninja installed, add -G "Ninja"
cmake -DCMAKE_TOOLCHAIN_FILE="%NDK_ROOT%\build\cmake\android.toolchain.cmake" ^
      -DANDROID_ABI=armeabi-v7a ^
      -DANDROID_PLATFORM=android-21 ^
      -DANDROID_STL=c++_static ^
      -DOpenCV_DIR="%OPENCV_DIR%" ^
      -DCMAKE_BUILD_TYPE=Release ^
      ..

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configuration failed.
    pause
    exit /b 1
)

:: Build
echo.
echo Starting Build...
cmake --build . --config Release -- -j4

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    pause
    exit /b 1
)

echo.
echo [SUCCESS] Build complete!
echo Executable is located at: %~dp0%BUILD_DIR%\main
echo.
echo To deploy to device:
echo   adb push %~dp0%BUILD_DIR%\main /data/local/tmp/rk3288_engine
echo   adb shell "chmod +x /data/local/tmp/rk3288_engine"
echo   adb shell "/data/local/tmp/rk3288_engine"
echo.
pause
@echo off
setlocal

rem 入口可从其他目录调用，源码和产物始终锚定仓库。
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
cd /d "%REPO_ROOT%" || exit /b 1

:: --- CONFIGURATION START ---

if defined ANDROID_SDK_ROOT (
    set "SDK_ROOT=%ANDROID_SDK_ROOT%"
) else if defined ANDROID_HOME (
    set "SDK_ROOT=%ANDROID_HOME%"
) else (
    set "SDK_ROOT="
)

:: 显式NDK优先；不能在括号块内放跳转标签，否则cmd会误解析右括号。
set "NDK_ROOT=%ANDROID_NDK_HOME%"
if not defined NDK_ROOT (
    if not defined SDK_ROOT (
        echo [ERROR] 请设置 ANDROID_NDK_HOME 或 Android SDK 根目录。
        exit /b 1
    )
    for /f "delims=" %%d in ('dir "%SDK_ROOT%\ndk" /b /ad 2^>nul ^| findstr /r "^[0-9]" ^| sort /r') do if not defined NDK_ROOT set "NDK_ROOT=%SDK_ROOT%\ndk\%%d"
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
set "BUILD_DIR=%REPO_ROOT%\cache\builds\android-opencv-ninja-armeabi-v7a"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

set "PYTHONDONTWRITEBYTECODE=1"
set "PYTHONNOUSERSITE=1"
set "PYTHONPYCACHEPREFIX=%BUILD_DIR%\pycache"

echo ==============================================
echo  Building for RK3288 (ARM Cortex-A17)
echo  ABI: armeabi-v7a (32-bit)
echo  STL: c++_static
echo ==============================================

:: Configure CMake
:: Note: We use "NMake Makefiles" or default generator. If you have Ninja installed, add -G "Ninja"
"%CMAKE_EXE%" -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G "Ninja" -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
      -DCMAKE_TOOLCHAIN_FILE="%NDK_ROOT%\build\cmake\android.toolchain.cmake" ^
      -DANDROID_ABI=armeabi-v7a ^
      -DANDROID_PLATFORM=android-21 ^
      -DANDROID_STL=c++_static ^
      -DOPENCV_ROOT="%OPENCV_ROOT_PATH%" ^
      -DOPENCV_CONTRIB_ROOT="%OPENCV_CONTRIB_ROOT_PATH%" ^
      -DWITH_IPP=OFF ^
      -DOPENCV_DOWNLOAD_IPPICV=OFF ^
      -DWITH_FFMPEG=OFF ^
      -DCMAKE_BUILD_TYPE=Release ^
      > "%BUILD_DIR%\build_config.log" 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configuration failed.
    type "%BUILD_DIR%\build_config.log"
    exit /b 1
)

:: Build
echo.
echo Starting Build...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target opencv_verify_cli -- -j4 > "%BUILD_DIR%\build.log" 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    type "%BUILD_DIR%\build.log"
    exit /b 1
)

echo.
echo [SUCCESS] Build complete!
echo Executable is located at: %BUILD_DIR%\bin\opencv_verify_cli
echo.
echo To deploy to device:
echo   adb push %BUILD_DIR%\bin\opencv_verify_cli /data/local/tmp/opencv_verify_cli
echo   adb shell "chmod +x /data/local/tmp/opencv_verify_cli"
echo   adb shell "/data/local/tmp/opencv_verify_cli --input /data/local/tmp/tests --cascade /data/local/tmp/lbpcascade_frontalface.xml"
echo.

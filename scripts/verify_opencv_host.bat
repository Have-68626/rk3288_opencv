@echo off
setlocal EnableDelayedExpansion

echo ==============================================
echo  OpenCV 4.10.0 Host Verification Script
echo ==============================================

set "BUILD_DIR=build_host"
if not defined OPENCV_ROOT (
    echo [ERROR] OPENCV_ROOT 未设置。请设置环境变量 OPENCV_ROOT 指向 OpenCV 源码根目录。
    exit /b 1
)

if defined ANDROID_SDK_ROOT (
    set "SDK_ROOT=%ANDROID_SDK_ROOT%"
) else if defined ANDROID_HOME (
    set "SDK_ROOT=%ANDROID_HOME%"
) else (
    set "SDK_ROOT="
)
set "CMAKE_EXE="

if not "%SDK_ROOT%"=="" if exist "%SDK_ROOT%/cmake/4.1.2/bin/cmake.exe" set "CMAKE_EXE=%SDK_ROOT%/cmake/4.1.2/bin/cmake.exe"
if "%CMAKE_EXE%"=="" if not "%SDK_ROOT%"=="" if exist "%SDK_ROOT%/cmake/3.22.1/bin/cmake.exe" set "CMAKE_EXE=%SDK_ROOT%/cmake/3.22.1/bin/cmake.exe"
if "%CMAKE_EXE%"=="" set "CMAKE_EXE=cmake"

if not exist "%OPENCV_ROOT%" (
    echo [ERROR] OpenCV root not found: %OPENCV_ROOT%
    exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

set "PYTHONDONTWRITEBYTECODE=1"
set "PYTHONNOUSERSITE=1"
set "PYTHONPYCACHEPREFIX=%CD%\pycache"

echo [INFO] Configuring CMake...
"%CMAKE_EXE%" -G "Visual Studio 17 2022" -A x64 ^
    -DOPENCV_ROOT="%OPENCV_ROOT%" ^
    -DOPENCV_CONTRIB_ROOT="%OPENCV_CONTRIB_ROOT%" ^
    -DBUILD_SHARED_LIBS=OFF ^
    -DWITH_IPP=OFF ^
    -DOPENCV_DOWNLOAD_IPPICV=OFF ^
    -DWITH_FFMPEG=OFF ^
    -DOPENCV_FFMPEG_SKIP_DOWNLOAD=ON ^
    -DBUILD_LIST=core,imgproc,imgcodecs,objdetect,features2d ^
    -DCPU_BASELINE= ^
    -DCPU_DISPATCH= ^
    .. > build_config.log 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configuration failed.
    type build_config.log
    exit /b 1
)

echo [INFO] Building Project...
"%CMAKE_EXE%" --build . --config Release --target opencv_verify_cli -j 4 > build.log 2>&1

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed.
    type build.log
    exit /b 1
)

echo [INFO] Build Successful. Starting Functional Tests...
cd ..

set "EXE_PATH=%BUILD_DIR%\bin\Release\opencv_verify_cli.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%BUILD_DIR%\bin\Debug\opencv_verify_cli.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%BUILD_DIR%\Release\opencv_verify_cli.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%BUILD_DIR%\Debug\opencv_verify_cli.exe"
if not exist "%EXE_PATH%" (
    echo [ERROR] Executable not found.
    exit /b 1
)

set "TEST_DATA=tests\data"
set "CASCADE=%TEST_DATA%\lbpcascade_frontalface.xml"
set "STORAGE=host_storage"

if not exist "%STORAGE%" mkdir "%STORAGE%"
if exist test_results.txt del /q test_results.txt

if not exist "%TEST_DATA%\lena.bmp" (
    powershell -NoProfile -Command ^
        "$ErrorActionPreference='Stop';" ^
        "$src=Join-Path '%CD%' '%TEST_DATA%\\lena.jpg';" ^
        "$dst=Join-Path '%CD%' '%TEST_DATA%\\lena.bmp';" ^
        "Add-Type -AssemblyName System.Drawing;" ^
        "$img=[System.Drawing.Image]::FromFile($src);" ^
        "$img.Save($dst,[System.Drawing.Imaging.ImageFormat]::Bmp);" ^
        "$img.Dispose();" > nul 2>&1
)

echo [TEST] Running OpenCV verification on test dataset...
"%EXE_PATH%" --input "%TEST_DATA%" --cascade "%CASCADE%" --manifest "%TEST_DATA%\manifest.csv" --out "opencv_verify_results.csv" > verify.log 2>&1
findstr /C:"VERIFY_SUMMARY" verify.log >nul
if %ERRORLEVEL% equ 0 (
    echo [PASS] Verification finished.
    type verify.log
) else (
    echo [FAIL] Verification failed.
    type verify.log
    exit /b 1
)

echo [INFO] Verification Complete.

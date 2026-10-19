@echo off
setlocal EnableDelayedExpansion

rem 入口可从其他目录调用，源码和产物始终锚定仓库。
for %%I in ("%~dp0..") do set "REPO_ROOT=%%~fI"
set "INPUT_DIR="
if not "%~1"=="" for %%I in ("%~1") do set "INPUT_DIR=%%~fI"
set "CASCADE="
if not "%~2"=="" for %%I in ("%~2") do set "CASCADE=%%~fI"
set "OUT_DIR="
if not "%~3"=="" for %%I in ("%~3") do set "OUT_DIR=%%~fI"
set "VIS_DIR="
if not "%~5"=="" for %%I in ("%~5") do set "VIS_DIR=%%~fI"
cd /d "%REPO_ROOT%" || exit /b 1

set "MODE=%~4"
set "NMS_IOU=%~6"

if "%INPUT_DIR%"=="" set "INPUT_DIR=tests\test_set01"
if "%CASCADE%"=="" set "CASCADE=tests\data\lbpcascade_frontalface.xml"
if "%OUT_DIR%"=="" set "OUT_DIR=%REPO_ROOT%\cache\reports\test-set01"
if "%MODE%"=="" set "MODE=default"
if "%NMS_IOU%"=="" set "NMS_IOU=0"

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

set "BUILD_DIR=%REPO_ROOT%\cache\builds\windows-opencv-vs2022-x64"

if not defined OPENCV_ROOT (
    echo [ERROR] OPENCV_ROOT 未设置。请设置环境变量 OPENCV_ROOT 指向 OpenCV 源码根目录。
    exit /b 1
)

set "CMAKE_EXE="
if defined ANDROID_SDK_ROOT if exist "%ANDROID_SDK_ROOT%/cmake/4.1.2/bin/cmake.exe" set "CMAKE_EXE=%ANDROID_SDK_ROOT%/cmake/4.1.2/bin/cmake.exe"
if "%CMAKE_EXE%"=="" if defined ANDROID_SDK_ROOT if exist "%ANDROID_SDK_ROOT%/cmake/3.22.1/bin/cmake.exe" set "CMAKE_EXE=%ANDROID_SDK_ROOT%/cmake/3.22.1/bin/cmake.exe"
if "%CMAKE_EXE%"=="" if defined ANDROID_HOME if exist "%ANDROID_HOME%/cmake/4.1.2/bin/cmake.exe" set "CMAKE_EXE=%ANDROID_HOME%/cmake/4.1.2/bin/cmake.exe"
if "%CMAKE_EXE%"=="" if defined ANDROID_HOME if exist "%ANDROID_HOME%/cmake/3.22.1/bin/cmake.exe" set "CMAKE_EXE=%ANDROID_HOME%/cmake/3.22.1/bin/cmake.exe"
if "%CMAKE_EXE%"=="" set "CMAKE_EXE=cmake"

if not exist "%OPENCV_ROOT%" (
    echo [ERROR] OpenCV root not found: %OPENCV_ROOT%
    exit /b 1
)
if not defined OPENCV_CONTRIB_ROOT (
    echo [ERROR] OPENCV_CONTRIB_ROOT 未设置。请设置环境变量指向 opencv_contrib 源码目录。
    exit /b 1
)
if not exist "%OPENCV_CONTRIB_ROOT%" (
    echo [ERROR] OpenCV contrib root not found: %OPENCV_CONTRIB_ROOT%
    exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"


if not exist "%BUILD_DIR%\CMakeCache.txt" (
    "%CMAKE_EXE%" -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 ^
        -DOPENCV_ROOT="%OPENCV_ROOT%" ^
        -DOPENCV_CONTRIB_ROOT="%OPENCV_CONTRIB_ROOT%" ^
        -DBUILD_SHARED_LIBS=OFF ^
        -DWITH_IPP=OFF ^
        -DOPENCV_DOWNLOAD_IPPICV=OFF ^
        -DWITH_FFMPEG=OFF ^
        -DOPENCV_FFMPEG_SKIP_DOWNLOAD=ON ^
        -DCPU_BASELINE= ^
        -DCPU_DISPATCH= ^
        > "%BUILD_DIR%\build_config.log" 2>&1

    if errorlevel 1 (
        echo [ERROR] CMake configuration failed.
        type "%BUILD_DIR%\build_config.log"
        exit /b 1
    )
)

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target opencv_verify_cli -j 4 > "%BUILD_DIR%\build_verify_cli.log" 2>&1
if errorlevel 1 (
    echo [ERROR] Build failed.
    type "%BUILD_DIR%\build_verify_cli.log"
    exit /b 1
)

set "EXE_PATH=%BUILD_DIR%\bin\Release\opencv_verify_cli.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%BUILD_DIR%\Release\opencv_verify_cli.exe"

if not exist "%EXE_PATH%" (
    echo [ERROR] Executable not found: %EXE_PATH%
    exit /b 1
)


set "CSV_PATH=%OUT_DIR%\face_results.csv"
set "JSON_PATH=%OUT_DIR%\face_results.json"
set "RUN_LOG=%OUT_DIR%\run.log"
set "SCALE=1.1"
set "NEIGHBORS=3"
set "MIN_SIZE=30"
set "MAX_SIZE=0"
set "EXTRA_ARGS="

set "MANIFEST_PATH=%INPUT_DIR%\manifest.csv"
set "MANIFEST_ARG="
if exist "%MANIFEST_PATH%" set "MANIFEST_ARG=--manifest "%MANIFEST_PATH%""

if /I "%MODE%"=="tuned" (
    set "NEIGHBORS=8"
    set "MIN_SIZE=60"
    set "CSV_PATH=%OUT_DIR%\face_results_tuned_n8_min60.csv"
    set "JSON_PATH=%OUT_DIR%\face_results_tuned_n8_min60.json"
    set "RUN_LOG=%OUT_DIR%\run_tuned_n8_min60.log"
) else if /I "%MODE%"=="nms" (
    set "NEIGHBORS=8"
    set "MIN_SIZE=60"
    if "%NMS_IOU%"=="0" set "NMS_IOU=0.30"
    if "%VIS_DIR%"=="" set "VIS_DIR=%OUT_DIR%\vis_nms_n8_min60_iou!NMS_IOU!"
    set "CSV_PATH=%OUT_DIR%\face_results_tuned_n8_min60_nms_iou!NMS_IOU!.csv"
    set "JSON_PATH=%OUT_DIR%\face_results_tuned_n8_min60_nms_iou!NMS_IOU!.json"
    set "RUN_LOG=%OUT_DIR%\run_tuned_n8_min60_nms_iou!NMS_IOU!.log"
) else if /I "%MODE%"=="grid" (
    set "GRID_DIR=%OUT_DIR%\grid"
    if not exist "!GRID_DIR!" mkdir "!GRID_DIR!"
    if "%VIS_DIR%"=="" set "VIS_DIR=!GRID_DIR!\vis"
    set "PARAM_LIST=%REPO_ROOT%\tests\fixtures\opencv\face-parameters.csv"
    set "CSV_PATH=!GRID_DIR!\grid_summary.csv"
    set "JSON_PATH="
    set "RUN_LOG=!GRID_DIR!\run_grid.log"

    "%EXE_PATH%" --input "%INPUT_DIR%" --cascade "%CASCADE%" %MANIFEST_ARG% --param-list "!PARAM_LIST!" --top 10 --out "!CSV_PATH!" --vis-dir "!VIS_DIR!" > "!RUN_LOG!" 2>&1
    set "RC=!ERRORLEVEL!"
    findstr /C:"VERIFY_GRID_START" "!RUN_LOG!"
    echo [INFO] GRID_SUMMARY: !CSV_PATH!
    echo [INFO] GRID_DIR: !GRID_DIR!
    echo [INFO] VIS_DIR: !VIS_DIR!
    echo [INFO] LOG: !RUN_LOG!
    exit /b !RC!
)

set "EXTRA_ARGS=%EXTRA_ARGS% --scale %SCALE% --neighbors %NEIGHBORS% --min %MIN_SIZE% --max %MAX_SIZE%"
if not "%VIS_DIR%"=="" set "EXTRA_ARGS=%EXTRA_ARGS% --vis-dir ""%VIS_DIR%"""
if not "%NMS_IOU%"=="0" set "EXTRA_ARGS=%EXTRA_ARGS% --nms-iou %NMS_IOU%"

"%EXE_PATH%" --input "%INPUT_DIR%" --cascade "%CASCADE%" %MANIFEST_ARG% --out "%CSV_PATH%" --json "%JSON_PATH%" %EXTRA_ARGS% > "%RUN_LOG%" 2>&1
set "RC=!ERRORLEVEL!"

findstr /C:"VERIFY_SUMMARY" "%RUN_LOG%"
echo [INFO] CSV: %CSV_PATH%
echo [INFO] JSON: %JSON_PATH%
echo [INFO] LOG: %RUN_LOG%
if not "%VIS_DIR%"=="" echo [INFO] VIS_DIR: %VIS_DIR%

exit /b !RC!


# CLAUDE.md — 项目 AI 助手指南

## 项目简介
rk3288_opencv — 面向 Rockchip RK3288 平台的多模态人脸识别系统。

## 四个独立子系统
- Android App (Java/Gradle) — app/ + src/java/
- Windows 服务 (C++/CMake) — src/win/
- Web SPA (React/TypeScript) — web/
- C++ 核心引擎 — src/cpp/

## 构建命令
- core_unit_tests: cmake -S . -B build_ci -G Ninja -DRK_SKIP_OPENCV=ON && cmake --build build_ci --target core_unit_tests
- Windows 全量: cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT=... -DOPENCV_CONTRIB_ROOT=...
- Web: pnpm -C web install && pnpm -C web build
- Android: gradlew.bat --no-daemon :app:assembleDebug

## 测试框架
使用 GTest（自 Batch 5 起）。TEST(Group, Name) 宏。
测试文件在 tests/cpp/ 和 tests/win/ 目录。

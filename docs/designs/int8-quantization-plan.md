# INT8 量化工具链 Implementation Plan

> **状态**: ✅ 全部任务已完成。本文档保留为历史跟踪记录，行号以完成时为准。: Use compose:subagent (recommended) or compose:execute to implement this
plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立 ncnn INT8 量化工具链，支持 YOLO Face / ArcFace / MobileFaceNet 三个模型的 FP32→INT8 转换、运行时切换和精度验证。

**Architecture:** Python 量化脚本调用 ncnn2int8 工具，ModelRegistry 内建注册 INT8 模型变体，WinConfig 增加 int8Enabled 开关，FaceInferStages
根据配置自动选择 INT8/FP32 模型，face_infer_unit_tests 新增精度对比测试。

**Tech Stack:** Python 3.8+、ncnn（量化工具 + C++ 推理）、C++17、CMake、自定义 C++ bool 函数（非 Google Test）

---

## Task 1: 量化脚本 `scripts/quantize_ncnn_int8.py`

**Covers:** [S2]

**Files:**
- Create: `scripts/quantize_ncnn_int8.py`

- [x] **Step 1: 创建量化脚本骨架**
- [x] **Step 2: 验证脚本可执行**
- [x] **Step 3: Commit** (`b8218a0`)

---

## Task 2: ModelRegistry INT8 模型注册

**Covers:** [S3]

**Files:**
- Modify: `src/cpp/src/ModelRegistry.cpp:21-62`（`ensureBuiltinRegistered` 函数末尾）
- Modify: `src/cpp/include/ModelRegistry.h`（新增 `fileExists` 辅助函数）

- [x] **Step 1: 在 ModelRegistry.cpp 末尾添加 INT8 模型注册**
- [x] **Step 2: 运行 core_unit_tests 验证无编译错误**
- [x] **Step 3: Commit** (`9858c97`)

---

## Task 3: WinConfig 增加 int8Enabled 字段

**Covers:** [S3]

**Files:**
- Modify: `src/win/include/rk_win/WinConfig.h:98-105`（`ModelConfig` 结构体）

- [x] **Step 1: 在 ModelConfig 中添加 int8Enabled 字段**
- [x] **Step 2: 运行 win_unit_tests 验证无编译错误**
- [x] **Step 3: Commit** (`3457d7e`)

---

## Task 4: WinJsonConfig 支持 int8Enabled 读写

**Covers:** [S3]

**Files:**
- Modify: `src/win/src/WinJsonConfig.cpp:719-727`（JSON 序列化）
- Modify: `src/win/src/WinJsonConfig.cpp:937-948`（JSON 反序列化）

- [x] **Step 1: 在 JSON 序列化中添加 int8Enabled**
- [x] **Step 2: 在 JSON 反序列化中添加 int8Enabled**
- [x] **Step 3: 运行 win_unit_tests 验证**
- [x] **Step 4: Commit** (`398d4b3`)

---

## Task 5: FaceInferStages INT8 模型选择

**Covers:** [S3]

**Files:**
- Modify: `src/cpp/src/FaceInferStages.cpp:219-237`（检测器选择逻辑）
- Modify: `src/cpp/src/FaceInferStages.cpp:330-350`（识别器选择逻辑，需确认行号）

- [x] **Step 1: 修改检测器选择逻辑，支持 INT8**
- [x] **Step 2: 修改识别器选择逻辑，支持 INT8**
- [x] **Step 3: 在 FaceInferencePipeline.h 的 FaceInferRequest 中添加 int8Enabled 字段**
- [x] **Step 4: 运行 face_infer_unit_tests 验证**
- [x] **Step 5: Commit** (`7bfd217`)

---

## Task 6: INT8 精度验证测试

**Covers:** [S4]

**Files:**
- Create: `tests/cpp/test_int8_quantization.cpp`
- Modify: `CMakeLists.txt`（将测试加入 face_infer_unit_tests 目标）

- [x] **Step 1: 创建 INT8 精度验证测试文件**（已创建，使用自定义 bool 函数而非 GTest）
- [x] **Step 2: 将测试文件加入 CMakeLists.txt 并注册到 face_infer_unit_tests_main.cpp**（已实现）
- [x] **Step 3: 编译验证** — 构建 face_infer_unit_tests 确认编译通过
- [x] **Step 4: 运行测试（INT8 模型不存在时应跳过）**
- [x] **Step 5: Commit**

---

## Task 7: CI 集成量化与测试步骤

**Closes:** [S4]

**Files:**
- Modify: `.github/workflows/ci.yml`（Windows job 新增量化步骤）

- [x] **Step 1: 在 Windows job 中添加量化和 INT8 测试步骤**
- [x] **Step 2: 验证 CI 语法**
- [x] **Step 3: Commit**

---

## Task 8: 更新 AGENTS.md 文档

**Files:**
- Modify: `AGENTS.md`

- [x] **Step 1: 在 AGENTS.md 中添加 INT8 相关说明**（已在 `AGENTS.md` Architecture notes 中包含）
- [x] **Step 2: Commit**（随 Task 6+7 一起提交）

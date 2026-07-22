# INT8 量化工具链 Implementation Plan

> **最后复核**: 2026-07-21
>
> **状态**: 历史任务记录。原任务曾按 `yolo_face` 路径完成；当前量化脚本和 CI 已转向 SCRFD，但 C++ 运行时与测试尚未完成同名迁移，不能再视为端到端全部完成。

## 当前实现校准

| 范围 | 当前事实 | 后续完成条件 |
|---|---|---|
| Python 量化 | 支持 `scrfd`、`arcface`、`mobilefacenet`，使用 `ncnn2table` + `ncnn2int8` | 保持脚本参数与 CI 命令一致 |
| CI 检测主线 | 使用 `models/scrfd_ncnn` → `models/scrfd_int8_ncnn` | 删除当前未支持的 `--size` 参数，并让有效量化结果可验证 |
| C++ 检测 INT8 | 仍注册/读取 `yolo_face_int8` 历史路径 | 迁移到 SCRFD 适配器、ID、模型路径与加载参数 |
| 精度测试 | 模型缺失时跳过；独立检测测试仍读 `yolo_face` | 使用真实 SCRFD 模型执行并保存精度报告 |

以下勾选项只表示历史任务当时完成，不代表上述迁移项已完成。

**Goal（历史）:** 建立 ncnn INT8 量化工具链，支持 YOLO Face / ArcFace / MobileFaceNet 三个模型的 FP32→INT8 转换、运行时切换和精度验证。

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

> 复核备注：该任务只表示步骤已加入 CI。当前步骤为非阻断，且命令包含脚本未支持的 `--size`，不能作为量化成功或精度达标证据。

---

## Task 8: 更新 AGENTS.md 文档

**Files:**
- Modify: `AGENTS.md`

- [x] **Step 1: 在 AGENTS.md 中添加 INT8 相关说明**（已在 `AGENTS.md` Architecture notes 中包含）
- [x] **Step 2: Commit**（随 Task 6+7 一起提交）

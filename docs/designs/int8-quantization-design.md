# INT8 量化工具链设计文档

**日期**: 2026-06-21
**优先级**: P0（加速方案落地）
**方案**: 集成流水线（方案 B）
**实现状态**: ✅ 已实现（`scripts/quantize_ncnn_int8.py` + 8 项精度测试通过）

---

## [S1] 架构总览

INT8 量化流水线由 5 个组件组成：

```
tests/test_set01/          scripts/quantize_ncnn_int8.py     models/
  (校准图片)  ──────►  (ncnn2int8 量化)  ──────►  yolo_face_int8.param/.bin
                                                       arcface_int8.param/.bin
                                                       mobilefacenet_int8.param/.bin

ModelRegistry             FaceInferStages              face_infer_unit_tests
  (INT8 模型注册)  ──────►  (运行时选择)  ──────►  (精度验证 FP32 vs INT8)
```

**数据流**：
1. `scripts/quantize_ncnn_int8.py` 读取 FP32 模型 + 校准图片 → 输出 INT8 `.param`/`.bin`
2. `ModelRegistry` 内建注册 `yolo_face_int8` / `arcface_int8` / `mobilefacenet_int8`
3. 配置文件 `model.int8Enabled=true` 时，`FaceInferStages` 自动选择 INT8 模型
4. `face_infer_unit_tests` 新增 INT8 精度对比测试

---

## [S2] 量化脚本 `scripts/quantize_ncnn_int8.py`

**功能**：将 FP32 ncnn 模型转换为 INT8 量化模型

**命令行接口**：
```bash
python scripts/quantize_ncnn_int8.py \
  --model yolo_face \                    # 模型名（yolo_face / arcface / mobilefacenet）
  --fp32-dir models/yolo_face_ncnn \     # FP32 模型目录（含 .param + .bin）
  --calib-dir tests/test_set01 \         # 校准图片目录
  --output-dir models/yolo_face_int8_ncnn \  # INT8 输出目录
  --size 640 \                           # 输入尺寸（YOLO=640, ArcFace/MobileFaceNet=112）
  --num-samples 100                      # 校准图片采样数
```

**实现要点**：
- 使用 ncnn Python 绑定或 subprocess 调用 ncnn2int8 CLI
- 校准图片自动 resize 到目标尺寸，转 RGB，归一化到 [0,1]
- 输出 INT8 `.param`（层名含 `quant` 标记）+ `.bin`（权重已量化）
- 生成 `quantize_report.json`：记录每层量化前后参数范围、压缩比

**依赖**：Python 3.8+、ncnn Python 绑定（或 ncnn2int8 CLI）

---

## [S3] ModelRegistry INT8 模型注册

**新增内建注册**（`ModelRegistry.cpp`）：

```cpp
// INT8 模型注册 — 需要 INT8 模型文件存在才注册
if (fileExists("models/yolo_face_int8_ncnn/yolo_face_int8.param")) {
    registerDetector("yolo_face_int8", []{ return CreateNcnnYoloFaceDetector("yolo_face_int8_ncnn"); });
}
if (fileExists("models/arcface_int8_ncnn/arcface_int8.param")) {
    registerEmbedder("arcface_int8", []{ return CreateNcnnArcFaceEmbedder("arcface_int8_ncnn"); });
}
if (fileExists("models/mobilefacenet_int8_ncnn/mobilefacenet_int8.param")) {
    registerEmbedder("mobilefacenet_int8", []{ return CreateMobileFaceNetEmbedder("mobilefacenet_int8_ncnn"); });
}
```

**配置开关**（`WinConfig.h`）：
```cpp
struct ModelConfig {
    bool int8Enabled = false;  // 新增：启用 INT8 推理
    // ... 已有字段
};
```

**JSON 持久化**（`WinJsonConfig.cpp`）：
- `model.int8Enabled` → JSON 读写 + schema 校验

**运行时逻辑**（`FaceInferStages.cpp`）：
```cpp
if (req.int8Enabled && ModelRegistry::instance().getEntry("yolo_face_int8").has_value()) {
    detector = ModelRegistry::instance().createDetector("yolo_face_int8");
} else {
    detector = ModelRegistry::instance().createDetector("yolo_face");
}
```

---

## [S4] 精度验证集成

**新增测试**（`tests/cpp/test_int8_quantization.cpp`）：

```cpp
TEST(Int8Quantization, LoadsYoloFaceInt8) {
    auto detector = ModelRegistry::instance().createDetector("yolo_face_int8");
    ASSERT_NE(detector, nullptr);
}

TEST(Int8Quantization, YoloFaceDetectionIou) {
    // FP32 vs INT8 检测框 IoU >= 0.7
}

TEST(Int8Quantization, ArcFaceEmbeddingSimilarity) {
    // FP32 vs INT8 特征余弦相似度 >= 0.9
}
```

**CMake 集成**：
- 加入 `face_infer_unit_tests` 目标
- 仅当 INT8 模型文件存在时运行（`RK_HAVE_INT8_MODELS` 宏控制）

**CI 集成**（`ci.yml` 新增 step）：
```yaml
- name: Quantize models
  run: python scripts/quantize_ncnn_int8.py --model yolo_face --calib-dir tests/test_set01
- name: Run INT8 precision tests
  run: cmake --build build_win --config Release --target face_infer_unit_tests && ctest -R int8
```

---

## [S5] 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|----------|------|
| `scripts/quantize_ncnn_int8.py` | 新增 | INT8 量化脚本 |
| `src/cpp/include/ModelRegistry.h` | 修改 | 新增 INT8 模型查询接口 |
| `src/cpp/src/ModelRegistry.cpp` | 修改 | 内建注册 INT8 模型 |
| `src/win/include/WinConfig.h` | 修改 | ModelConfig 增加 int8Enabled |
| `src/win/src/WinJsonConfig.cpp` | 修改 | int8Enabled JSON 读写 |
| `src/cpp/src/FaceInferStages.cpp` | 修改 | 运行时 INT8 模型选择 |
| `tests/cpp/test_int8_quantization.cpp` | 新增 | INT8 精度验证测试 |
| `CMakeLists.txt` | 修改 | 新增 test_int8_quantization 目标 |
| `.github/workflows/ci.yml` | 修改 | 新增量化 + INT8 测试 step |

# INT8 量化工具链设计文档

**日期**: 2026-06-21

**最后复核**: 2026-07-21

**优先级**: P0（加速方案落地）

**实现状态**: 部分落地；量化脚本主线已迁移到 SCRFD，C++ 运行时和测试仍保留 `yolo_face` 历史路径

## 1. 当前结论

- `scripts/quantize_ncnn_int8.py` 实现 `ncnn2table` → `ncnn2int8` 两步流程，支持 `scrfd`、`arcface`、`mobilefacenet`。
- 量化脚本与 Windows CI 的检测模型主线是 SCRFD；CI 输入为 `models/scrfd_ncnn`，输出为 `models/scrfd_int8_ncnn`。
- `ModelRegistry`、`FaceInferStages`、`test_int8_quantization.cpp` 与 `ncnn_precision_test.cpp` 仍使用 `yolo_face_int8` 和 `models/yolo_face*_ncnn`。这属于尚未完成的历史兼容路径，不能表述为已与 SCRFD 统一。
- `model.int8Enabled` 已进入 Windows 配置序列化和运行时选择逻辑；模型缺失时仍走既有回退路径。
- 测试在模型文件缺失时返回跳过；“测试通过”不等于完成 FP32/INT8 精度验证。独立精度测试的 ArcFace 余弦相似度阈值为 0.90。
- 当前 Windows CI 的量化命令包含脚本未定义的 `--size` 参数，且量化与精度步骤均为非阻断。因此当前 CI 不能证明量化成功或精度达标。

## 2. 实际流水线

```text
deps/WIDER_train/WIDER_train/images
                |
                v
scripts/quantize_ncnn_int8.py
  |- ncnn2table: 生成 <model>.table
  `- ncnn2int8: 生成 <model>_int8.param/.bin
                |
                v
models/scrfd_int8_ncnn（脚本/CI 主线）

models/yolo_face_int8_ncnn（C++ 注册与测试的历史路径）
```

量化报告 `quantize_report.json` 当前只记录 FP32/INT8 `.bin` 字节数与压缩比，不包含逐层参数范围或精度结论。

## 3. 量化脚本接口

```powershell
python scripts/quantize_ncnn_int8.py `
  --model scrfd `
  --fp32-dir models/scrfd_ncnn `
  --calib-dir deps/WIDER_train/WIDER_train/images `
  --output-dir models/scrfd_int8_ncnn `
  --num-samples 50
```

关键约束：

- `--model` 仅接受 `scrfd`、`arcface`、`mobilefacenet`。
- 完整流程和 `--table-only` 需要 `--calib-dir`；`--quant-only` 可用 `--table-path` 指定已有校准表。
- ncnn 工具从 `PATH`、`RK_NCNN_ROOT` 或 `NCNN_ROOT` 查找。
- 脚本自动选择目录中的第一对 `.param`/`.bin`，输出名固定为 `<model>_int8.param/.bin`。
- 脚本当前没有 `--size` 参数；输入尺寸来自内置模型预设。

## 4. 运行时与配置

- `WinConfig::ModelConfig::int8Enabled` 默认 `false`，并映射到 JSON 的 `model.int8Enabled`。
- 检测阶段启用 INT8 时，当前尝试创建 `yolo_face_int8`；识别阶段依次尝试 `arcface_int8`、`mobilefacenet_int8`。
- `ModelRegistry` 仅在对应历史路径模型文件存在时注册这些 ID。
- 在完成 SCRFD C++ 适配器、注册 ID、配置路径和测试路径迁移前，不得把 `scrfd_int8_ncnn` 直接视为运行时已消费的检测模型。

## 5. 测试与验收边界

`face_infer_unit_tests` 注册 8 个 INT8 相关自定义 `bool` 用例，覆盖模型注册/创建以及检测、ArcFace 精度入口。模型缺失时这些用例会跳过。`ncnn_precision_test` 是独立运行器，但检测模型仍读取历史 `yolo_face` 路径。

有效的“已验收”至少需要同时满足：

1. 量化命令无未知参数并成功生成模型与报告。
2. C++ 运行时、注册表和测试使用同一 SCRFD 路径与模型命名。
3. 测试输出明确显示实际执行而非模型缺失跳过。
4. 检测精度指标和 ArcFace 余弦相似度阈值均有真实模型、校准集和输出报告支撑。

在上述条件完成前，本工具链状态保持为“部分落地”。

# 人脸检测验证增强 Spec

## Why
当前基于 Cascade 的人脸检测在 `tests/test_set01` 上存在明显重复计数与漏检，无法达到“精确人脸数量准确率 ≥ 95%”的验证目标，需要引入可解释的可视化、去重与可重复的参数评估流程。

## What Changes
- 为现有离线检测工具补齐“按标注评估”的准确率指标输出（精确数量准确率、平均绝对误差、漏检/过计数统计）。
- 增加检测框可视化输出（在原图上绘制人脸框并落盘），用于人工复核标注与分析误差原因。
- 增加检测框去重/合并策略（NMS 或等价方案），减少同一人脸被重复统计导致的过计数。
- 提供可重复的参数评估能力（对 `scaleFactor/minNeighbors/minSize/maxSize` 的组合评估与排名输出）。
- 更新 `tests/reports/test_set01` 的报告内容，明确默认参数与调优参数对比结论，以及未达标时的下一步路径（模型替换/DNN）。

## Impact
- Affected specs: 离线人脸检测验证、准确率量化、可视化复核、参数调优流程
- Affected code:
  - `src/cpp/opencv_verify_cli.cpp`
  - `scripts/verify_faces_test_set01.bat`
  - `tests/test_set01/manifest.csv`
  - `tests/reports/test_set01/*`

## ADDED Requirements
### Requirement: 标注对比评估
系统 SHALL 支持读取 `manifest.csv`（字段：`filename,expected_faces`）并输出以下评估指标：
- 精确数量准确率：`faces == expected_faces` 的比例
- 平均绝对误差：`avg_abs_error`
- 过计数/漏检样本清单（至少输出文件名与误差）

#### Scenario: 成功评估输出
- **WHEN** 用户对 `tests/test_set01` 运行验证工具并提供 manifest
- **THEN** 输出的 CSV/JSON SHALL 包含每张图片的 `expected_faces/match/abs_error`
- **AND** summary SHALL 包含 `exact_accuracy/avg_abs_error` 等核心统计

### Requirement: 检测框可视化
系统 SHALL 支持为每张输入图片输出一张可视化结果图，包含：
- 人脸框（矩形）
- 人脸序号（1..N）
- 文件名与检测数量摘要（可选）

#### Scenario: 可视化用于人工复核
- **WHEN** 用户开启可视化输出并运行在 test_set01
- **THEN** 输出目录 SHALL 生成与输入同名的可视化图片文件
- **AND** 任意失败（加载失败/写文件失败） SHALL 被记录到结构化结果中

### Requirement: 检测框去重/合并（减少重复计数）
系统 SHALL 提供可选的去重/合并策略用于计数稳定化：
- NMS（基于 IoU 阈值）或等价合并方案
- 可配置阈值（例如 `--nms-iou <float>`）

#### Scenario: 去重后计数更稳定
- **WHEN** 在同一输入上开启去重/合并
- **THEN** 结构化结果中的 faces 数量 SHALL 反映去重后的计数
- **AND** 在 `test_set01` 上精确数量准确率 SHOULD 提升或保持不下降

### Requirement: 参数评估与排名
系统 SHALL 支持批量评估一组参数组合，并输出排名表（CSV 或 Markdown）：
- 每组参数的 `exact_accuracy/avg_abs_error/avg_ms_detect`
- Top-N 参数组合及对应结果文件路径

#### Scenario: 网格评估
- **WHEN** 用户指定参数组合列表或预设网格范围
- **THEN** 系统生成汇总表并标记最佳参数组合

## MODIFIED Requirements
### Requirement: 结构化结果字段最小集
现有结构化结果输出 SHALL 至少包含：
- `filename`
- `has_face`
- `faces`
- `ms_detect`
- `ok`
- `err`
并在存在标注时补齐：
- `expected_faces`
- `match`
- `abs_error`

## REMOVED Requirements
### Requirement: 无
**Reason**: 无需移除既有能力，仅增强验证与评估链路。
**Migration**: 兼容旧用法；新增参数均提供默认值，不破坏原有脚本执行。


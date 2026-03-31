# Tasks

- [x] Task 1: 对齐 test_set01 标注与评估口径
  - [x] 确认 `tests/test_set01/manifest.csv` 覆盖全部样本且与用户口径一致
  - [x] 确认评估指标定义：精确数量准确率、平均绝对误差、漏检/过计数样本

- [x] Task 2: 增强 opencv_verify_cli 的评估输出与可配置参数
  - [x] 输出 CSV/JSON 增加 `expected_faces/match/abs_error`
  - [x] summary 输出 `exact_accuracy/avg_abs_error` 与关键计数统计
  - [x] 参数化 detectMultiScale：`scale/neighbors/min/max`

- [x] Task 3: 增加检测框可视化输出（落盘）
  - [x] 增加 `--vis-dir <path>` 或等价参数，输出绘制人脸框的结果图
  - [x] 在结构化结果中记录可视化输出失败原因（如写文件失败）

- [x] Task 4: 增加检测框去重/合并策略（NMS/聚类合并）
  - [x] 增加 `--nms-iou <float>` 与开关参数（关闭即保持现有行为）
  - [x] 确保 faces 计数来自去重后的框集合

- [x] Task 5: 参数评估与排名输出
  - [x] 增加 `--grid` 或 `--param-list <file>` 的批量评估入口（任选其一，优先简单可维护方案）
  - [x] 输出汇总表（CSV 或 Markdown），包含参数组合与关键指标

- [x] Task 6: 脚本与报告更新（可复现交付）
  - [x] 更新 `scripts/verify_faces_test_set01.bat` 支持：可视化输出目录、去重参数、参数评估（如启用）
  - [x] 更新 `tests/reports/test_set01/face_report.md`：默认 vs 调优 vs 去重的对比结论

- [x] Task 7: 验证与回归
  - [x] 在 `test_set01` 上跑默认参数、调优参数与去重方案，保存全部结果文件
  - [x] 若 `exact_accuracy < 0.95`，在报告中给出明确下一步（模型替换 Haar/DNN）与推荐命令

# Task Dependencies
- Task 4 depends on Task 2
- Task 6 depends on Task 2/3/4/5
- Task 7 depends on Task 6

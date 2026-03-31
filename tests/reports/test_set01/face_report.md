# test_set01 人脸检测效果验证报告

## 1. 目标与范围

本次验证覆盖目录 `tests/test_set01` 内的全部图像样本，执行“是否存在人脸”与“人脸数量”检测输出，并记录耗时与失败原因（如有）。

说明：本报告验证的是“人脸检测（face detection）”，不包含身份级“人脸识别（face recognition）”。

## 2. 工具与配置

- 可执行程序：`opencv_verify_cli`
  - 源码：[opencv_verify_cli.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/opencv_verify_cli.cpp)
  - 构建目标配置：[CMakeLists.txt](file:///d:/19842/Documents/GitHub/rk3288_opencv/CMakeLists.txt)
- 使用的人脸模型：`lbpcascade_frontalface.xml`
  - 路径：`tests/data/lbpcascade_frontalface.xml`
- 一键脚本：[verify_faces_test_set01.bat](file:///d:/19842/Documents/GitHub/rk3288_opencv/scripts/verify_faces_test_set01.bat)

## 3. 结构化结果文件

- CSV：`tests/reports/test_set01/face_results.csv`
- JSON：`tests/reports/test_set01/face_results.json`
- 运行日志：`tests/reports/test_set01/run.log`
- 标注清单：`tests/test_set01/manifest.csv`

CSV 字段（满足需求的核心字段已包含）：
- `filename`：文件名
- `expected_faces`：标注人脸数量（整数，-1 表示未标注）
- `has_face`：是否存在人脸（1/0）
- `faces`：人脸数量（整数）
- `match`：是否与标注一致（1/0，仅在存在标注且 ok=1 时有效）
- `abs_error`：与标注的绝对误差（仅在存在标注且 ok=1 时有效）
- `ms_detect`/`ms_total`：检测耗时（毫秒）
- `ok`/`err`：失败标记与失败原因（字符串）
- `faces_raw`：去重前的人脸框数量（整数）
- `vis_ok`/`vis_err`：可视化输出成功标记与失败原因（字符串）

## 4. 汇总结论

- 总样本数：12
- 成功处理：12（失败 0，失败原因见 `err` 字段）
- 检测到人脸的样本数：10
- 检测出的人脸总数：26

性能统计（Host 环境，来自 `face_results.json` summary）：
- `avg_ms_detect`：37.83ms
- `avg_ms_total`：47.42ms

## 5. 逐图结果（是否有人脸 + 人脸数量）

| 文件名 | 是否有人脸 | 人脸数量 |
| :--- | :---: | ---: |
| 0c5560c8181cf16ee37eba5f5df5366d.jpg | 是 | 3 |
| 1e10fe630d9c08fef74e25b244430286.jpg | 是 | 1 |
| 1e543ddaff914c2bebd331d414f2c330.jpg | 是 | 4 |
| 20c642d47a95d8d5f749a0152fc5a022.jpg | 否 | 0 |
| 23426a5cf4e6895c568b2e9a574a6051.jpg | 是 | 8 |
| 7688d73c708e6b63e2e88553daab79bb.jpg | 是 | 2 |
| 85ac713abb983adc152344dcd041e565.jpg | 是 | 1 |
| acb1d67ee9e0a0084ea83db66bb7e400.jpg | 是 | 2 |
| b3ad136f94f44ec9a568afb27fbdbf06.jpg | 否 | 0 |
| bb97a5d147dc7f7c7badbb7c088f19c9.jpg | 是 | 1 |
| f1eeb626846207ee9a91b518e9f34eea.jpg | 是 | 2 |
| f6f8487478ad6a6be71773d6edcc1115.jpg | 是 | 2 |

## 6. 准确率与阈值说明（95%）

`tests/test_set01/manifest.csv` 已补齐标注（依据用户提供的口径：`7688d73c708e6b63e2e88553daab79bb.jpg` 为 2 张人脸，其余图片均为 1 张人脸）。

本次采用默认参数（LBP cascade：`scale=1.1`，`neighbors=3`，`min=30`）的量化结果（来自 `face_results.json` summary）：
- 精确数量准确率（faces == expected_faces）：4/12 = 33.33%
- 二分类（是否有人脸）召回：10/12 = 83.33%（本标注集全为正样本，故仅能观察漏检）
- 平均绝对误差：1.4167

结论：当前检测准确率远低于 95% 阈值，需要调参或更换检测模型。

## 7. 低准确率优化建议（调优方案）

### 7.1 已验证的参数调优（仍未达标）

已在 Host 侧尝试提高 `minNeighbors` 与 `minSize` 以抑制误检：
- 参数：`--scale 1.1 --neighbors 8 --min 60`
- 结果：精确数量准确率 9/12 = 75%（仍低于 95%）
- 主要误差样本：
  - `0c5560c8181cf16ee37eba5f5df5366d.jpg`：标注 1，检测 2（过计数）
  - `20c642d47a95d8d5f749a0152fc5a022.jpg`：标注 1，检测 0（漏检）
  - `b3ad136f94f44ec9a568afb27fbdbf06.jpg`：标注 1，检测 0（漏检）
- 输出文件：
  - `tests/reports/test_set01/face_results_tuned_n8_min60.csv`
  - `tests/reports/test_set01/face_results_tuned_n8_min60.json`

### 7.2 NMS 去重 + 可视化复核（已落地，仍未达标）

在检测框输出上增加可选 NMS（基于 IoU 阈值）去重，并支持输出逐图可视化结果图（绘制检测框 + 序号），用于人工复核：
- 复现命令：`scripts/verify_faces_test_set01.bat tests/test_set01 tests/data/lbpcascade_frontalface.xml tests/reports/test_set01 nms`
- 参数：`--neighbors 8 --min 60 --nms-iou 0.30 --vis-dir <dir>`
- 结果：精确数量准确率 9/12 = 75%（与调优参数一致，未达到 95%）
- 可视化输出目录：`tests/reports/test_set01/vis_nms_n8_min60_iou0.30`

### 7.3 参数批量评估与排名（网格评估）

已提供参数组合批量评估能力，并输出排名汇总表（CSV + Markdown）：
- 复现命令：`scripts/verify_faces_test_set01.bat tests/test_set01 tests/data/lbpcascade_frontalface.xml tests/reports/test_set01 grid`
- 参数列表：`scripts/face_param_list_test_set01.csv`
- 汇总排名：
  - CSV：`tests/reports/test_set01/grid/grid_summary.csv`
  - Markdown：`tests/reports/test_set01/grid/grid_summary.md`
- 当前 Top1（仍未达 95%）：`exact_accuracy = 0.75`，`avg_abs_error = 0.25`

### 7.4 建议的进一步优化路径（优先级从高到低）

- 误检抑制：在当前 Cascade 输出后增加“重叠框合并/NMS 去重”，避免同一张脸被重复计数（当前误差样本中存在明显过计数）。
- 多模型对比：对比 `haarcascade_frontalface_default.xml` / `haarcascade_frontalface_alt2.xml` 与当前 LBP cascade，在同一标注集上选择最佳模型与参数组合。
- 预处理改进：在强光/暗光样本上引入 CLAHE（局部直方图均衡）替代全局 `equalizeHist`。
- 升级到 DNN 检测：若必须达到 ≥95%，建议采用 DNN 人脸检测（如基于 SSD/RetinaFace 的轻量模型），并在 RK3288 上评估 CPU/RKNN 推理开销与帧率目标。

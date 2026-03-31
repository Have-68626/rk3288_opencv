# Tasks
- [x] Task 1: 补齐目标设备画像与约束清单
  - [x] 汇总 RK3288（Android 7.1.2）关键约束：ABI、内存、摄像头能力、GPU 可用性
  - [x] 明确“仅 CPU+GPU”前提下的性能目标与默认降级策略

- [x] Task 2: 推理框架实测对比并落定主选/备选
  - [x] 实现 ncnn 与 OpenCV DNN 的对比基准工具与统一口径（输入尺寸、warmup/iters、统计字段）
  - [x] 输出基准记录文件（CSV/JSON）格式与设备侧执行命令（实际数值由设备运行产出）

- [x] Task 3: 选定 YOLO 人脸检测模型形态并确定关键点策略
  - [x] 确认采用“带 5 点关键点输出”的 YOLO Face 变体，或补齐独立关键点模型方案
  - [x] 固定检测输入尺寸、NMS、score 阈值与输出 JSON 口径

- [x] Task 4: 选定 ArcFace 特征模型与模板格式（含加密存储）
  - [x] 固定 embedding 维度、归一化方式、dtype 与版本号
  - [x] 定义模板 schema 与落盘加密策略（Keystore 绑定、损坏处理与回滚口径）

- [x] Task 5: 定义 1:N 检索与阈值策略并给出可回滚口径
  - [x] 实现 TopK 检索与余弦相似度口径（先线性检索，后续再评估索引结构）
  - [x] 输出阈值版本化策略与“结果稳定”策略（连续 K 次通过等）

- [x] Task 6: 实现核心业务流程的端侧闭环原型
  - [x] 打通：取帧 → 检测 → 标框 → 对齐 → 特征 → 检索 → 事件输出 → 审计落盘
  - [x] 补齐：相机断连恢复、推理异常恢复、存储异常降级

- [x] Task 7: 提供基线采集工具与设备侧产出方式
  - [x] 在现有 CLI/管线基础上新增可复现的性能基线采集命令与报告模板（设备侧执行后自动产出 CSV + Markdown）
  - [x] 暴露关键参数到 CLI（检测频率/输入尺寸/多脸策略等），便于在设备侧复现实测

- [x] Task 8: 补齐许可证与第三方声明
  - [x] 在 `CREDITS.md` 登记新增第三方（推理框架、模型来源）与许可证
  - [x] 确保模型与数据集的来源、使用范围与脱敏策略可审计

# Task Dependencies
- Task 2 depends on Task 1
- Task 3 depends on Task 2
- Task 4 depends on Task 2
- Task 5 depends on Task 3/4
- Task 6 depends on Task 5
- Task 7 depends on Task 6
- Task 8 depends on Task 3/4

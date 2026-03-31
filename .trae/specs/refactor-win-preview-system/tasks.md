# Tasks

- [x] Task 1: 现状确认与接口口径对齐
  - [x] 明确“保持宽高比但无黑边”的实现策略（默认裁剪填充，提供 letterbox 可选）
  - [x] 定义 `faces` JSON 字段与坐标系口径（源帧坐标 + 显示坐标映射）
  - [x] 定义切换耗时、丢帧率、检测延迟与降级的统计口径

- [x] Task 2: UI 布局重构（响应式）
  - [x] 引入布局管理器：窗口缩放时控制面板与预览区自适应
  - [x] 预览区强制保持宽高比，提供裁剪填充/letterbox 两种模式切换

- [x] Task 3: 分辨率/FPS 动态切换与能力验证
  - [x] 增加分辨率下拉（1080p/720p/480p）与 FPS 选择（15/30/60）
  - [x] 基于相机能力验证组合可用性，不可用给出原因并回滚
  - [x] 切换时重启采集并重新初始化检测模型，统计重启耗时并落日志

- [x] Task 4: 轻量检测模型集成与叠加绘制
  - [x] 引入 DNN 检测实现（SSD MobileNet 优先），模型路径可配置
  - [x] 每帧推理（最新帧优先），输出 bbox/confidence/timestamp
  - [x] 叠加绘制：半透明白色描边 2px、圆角 5px，标注置信度百分比

- [x] Task 5: 性能监控与自动降级
  - [x] 监测检测延迟与丢帧率
  - [x] 超阈值自动降级（动态 stride），恢复后自动回升
  - [x] 记录降级/恢复事件与关键指标

- [x] Task 6: 结果输出端口（本地服务 + 外部 POST）
  - [x] 实现本地 HTTP 服务：GET /api/faces + 可选 SSE /api/faces/stream
  - [x] 实现外部 POST 推送（可配置 URL、节流、退避重试）
  - [x] 确保输出不阻塞渲染与采集线程

- [x] Task 7: 测试用例落地与报告
  - [x] 测试 A：窗口缩放 800×600→4K，布局无错位，画面不变形且无异常黑帧
  - [x] 测试 B：分辨率/FPS 组合各切换 10 次，重启耗时 <300ms 且无资源泄漏迹象（RSS/句柄/事件日志）
  - [x] 测试 C：使用“含 5 张人脸的视频源”连续运行 1 小时（允许使用虚拟摄像头/回放设备），确保输出端口数据格式一致、无漂移
  - [x] 输出报告文件与复现命令，归档到 `docs/windows-camera-face-recognition/`

- [x] Task 8: 文档与许可证更新
  - [x] 更新用户手册：新增分辨率/FPS、输出端口、降级策略说明
  - [x] 更新 DEVELOP.md：新增模块入口、配置项、测试复现路径
  - [x] 若引入模型文件或第三方库，更新 CREDITS.md（来源与许可证）

# Task Dependencies
- Task 3 depends on Task 2
- Task 4 depends on Task 3
- Task 5 depends on Task 4
- Task 6 depends on Task 4
- Task 7 depends on Task 2-6
- Task 8 depends on Task 1-7

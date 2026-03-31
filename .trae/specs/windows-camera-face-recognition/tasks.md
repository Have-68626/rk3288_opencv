# Tasks

- [x] Task 1: 明确 Windows 技术选型与依赖边界
  - [x] 确认采集后端：Media Foundation（默认）/ DirectShow（备选）
  - [x] 确认 UI 技术：Win32（默认）并规划控件与渲染方案
  - [x] 确认识别算法：OpenCV（默认）所需模块与构建开关；若需 opencv_contrib/face，明确集成方式
  - [x] 明确“≥95% 准确率”的评估数据集、口径与阈值策略

- [x] Task 2: Windows 摄像头接入模块
  - [x] 实现设备枚举（名称/ID/能力列表）
  - [x] 实现设备打开与分辨率/帧率配置
  - [x] 实现采集线程与帧交付接口（含错误分类与恢复建议）

- [x] Task 3: 实时预览窗口（Win32）
  - [x] 创建窗口与消息循环、UI 控件（相机选择/分辨率/翻转）
  - [x] 实现高效渲染路径（StretchDIBits 位图路径；预览/叠加在处理线程完成）
  - [x] 实现帧率统计与运行时切换稳定性

- [x] Task 4: 人脸识别核心库
  - [x] 实现人脸检测（多脸输出）
  - [x] 实现特征提取与比对（LBP 直方图 embedding + Chi-Square 距离）
  - [x] 实现注册库（enroll）与识别（identify），支持多人
  - [x] 提供阈值配置与调优入口（ini/命令行）

- [x] Task 5: UI 叠加与识别日志
  - [x] 在预览画面叠加框/ID/置信度
  - [x] 输出结构化日志（CSV/JSONL）并包含错误事件
  - [x] 提供日志目录与轮转/大小限制策略

- [x] Task 6: 性能与稳定性优化
  - [x] 建立采集-处理-渲染的线程隔离与背压策略（单槽最新帧，自动丢帧）
  - [x] 限制内存上限（单槽帧缓存 + 渲染帧；日志轮转）
  - [x] 加入异常恢复（采集失败计数与重连）

- [x] Task 7: 单元测试与评估工具
  - [x] 建立单元测试框架（CTest + 自包含测试可执行文件）
  - [x] 覆盖核心识别算法（embedding 维度/距离、混淆矩阵统计）
  - [x] 实现离线评估工具：输出准确率/误识/拒识/混淆矩阵

- [x] Task 8: 文档与交付
  - [x] 更新 `README.md`：Windows 测试系统简介与快速开始
  - [x] 更新 `DEVELOP.md`：目录树、环境变量、构建步骤、复现路径
  - [x] 更新 `CREDITS.md`：第三方依赖来源与许可证
  - [x] 提供用户操作手册与性能测试报告模板/基线（ZH_CN）

# Task Dependencies
- Task 2 depends on Task 1
- Task 3 depends on Task 2
- Task 4 depends on Task 1
- Task 5 depends on Task 3, Task 4
- Task 6 depends on Task 2, Task 3, Task 4
- Task 7 depends on Task 4
- Task 8 depends on Task 1-7

# Windows 分辨率适配与渲染稳定性增强 Spec

## Why
当前 Windows 预览程序需要在不同分辨率/刷新率/多显示器/高 DPI 环境下保持稳定渲染与可控性能。现阶段 Win32 预览存在闪屏/撕裂风险，且缺少“分辨率与刷新率组合”枚举、验证与应用内切换能力；同时需要补齐色彩、伽马、色温与 DPI 感知等显示质量能力，并提供可复现的长稳与性能基准验证手段。

## What Changes
- 新增“显示设置模块”：枚举显示器与可用显示模式（分辨率+刷新率），覆盖至少 640×480 到 4K。
- 新增“分辨率/刷新率组合验证”：基于系统能力校验可用性，并提供失败原因与可恢复提示。
- 新增“应用内实时切换”：在不重启程序的前提下，切换以下两类设置：
  - 预览渲染输出模式（窗口/全屏、渲染目标分辨率、VSync/帧同步策略）
  - 可选：系统显示模式（仅在用户显式开启全屏独占/显示模式切换时生效）
- 修复预览闪屏与撕裂：将渲染路径升级为 DXGI SwapChain（D3D11，双缓冲/三缓冲可选），并实现稳定的 Present 同步策略。
- 增加显示质量选项：抗锯齿（MSAA/FXAA 二选一，优先简单可控）、各向异性过滤（仅在纹理采样链路存在时启用，默认关闭）。
- 增加色彩与显示增强：sRGB 处理、伽马校正、色温调节；对 AdobeRGB/DCI-P3 提供“显示意图/转换策略”与能力检测（不强制承诺设备级色域准确，只保证链路可配置与可解释）。
- 增加高 DPI 适配：启用 Per-Monitor DPI Awareness v2，窗口布局随 DPI 变化正确缩放。
- 增加测试与验证：稳定性（多模式组合、多显示器）、72h 压测工具、性能基准（GPU 占用率、帧时间抖动）与报告模板/产出文件。

## Impact
- Affected specs: Windows 预览渲染、显示模式管理、性能与稳定性验证、配置与日志、文档交付
- Affected code（预期）:
  - `src/win/app/win_camera_face_recognition_main.cpp`（UI/设置入口）
  - `src/win/src/FramePipeline.cpp` / `FramePipeline.h`（渲染调度与帧同步）
  - 新增 `src/win/src/DisplaySettings.*`（显示枚举/验证/切换）
  - 新增 `src/win/src/D3D11Renderer.*`（SwapChain 渲染、双/三缓冲）
  - `config/windows_camera_face_recognition.ini`（新增显示相关配置）
  - `docs/windows-camera-face-recognition/*`（测试报告与操作手册更新）

## ADDED Requirements
### Requirement: 分辨率与刷新率动态调整系统
系统 SHALL 枚举并列出所有可用显示模式（分辨率+刷新率），并支持应用内实时切换。

#### Scenario: 枚举可用模式
- **WHEN** 用户打开“显示设置”面板
- **THEN** 系统展示当前显示器列表与每个显示器的可用模式列表
- **AND** 模式列表至少覆盖 640×480 至 3840×2160（如硬件支持）

#### Scenario: 模式组合验证
- **WHEN** 用户选择一个分辨率/刷新率组合
- **THEN** 系统在应用前进行验证，并在不可用时给出明确原因（不支持/被策略限制/多显示器冲突等）

#### Scenario: 应用内切换（不重启）
- **WHEN** 用户确认切换
- **THEN** 500ms 内完成切换或回滚到上一个可用模式
- **AND** 程序不崩溃且不会进入不可恢复的黑屏/卡死状态

### Requirement: 预览闪屏与撕裂修复
系统 SHALL 使用双缓冲或三缓冲 SwapChain 渲染，降低闪屏与撕裂，并提供与刷新率匹配的帧同步策略。

#### Scenario: 稳定预览
- **WHEN** 预览持续运行
- **THEN** 闪屏率低于 0.1%（以定义的监测口径统计）
- **AND** 帧时间抖动可记录并用于基准对比

### Requirement: 显示效果增强
系统 SHALL 提供以下显示质量能力，并可在运行时启用/禁用：
- 色彩空间处理：sRGB（默认），并对 AdobeRGB/DCI-P3 提供能力检测与策略说明
- 伽马校正：可配置 gamma 值
- 色温调节：可配置色温范围（例如 3000K~8000K）
- DPI 感知：Per-Monitor DPI Awareness v2
- 全屏/窗口切换：过渡过程不黑屏、不崩溃（动画为可选增强项）

### Requirement: 测试与验证
系统 SHALL 提供可复现的测试与报告产出：
- 至少 5 组分辨率+刷新率组合稳定性测试
- 多显示器一致性验证步骤
- 72 小时连续运行压测工具与指标采集（内存/句柄/帧时间）
- 性能基准报告：包含 GPU 占用率与帧时间稳定性（P50/P95/P99）

## MODIFIED Requirements
### Requirement: 错误处理与用户提示
显示/渲染链路错误（设备丢失、模式切换失败、SwapChain 重建失败） SHALL 被分类记录并给出用户友好提示，且具备自动恢复或回滚策略。

## REMOVED Requirements
### Requirement: 无
**Reason**: 本变更为增强显示适配与稳定性，不移除既有功能。
**Migration**: 默认保持窗口模式与现有行为；新增能力通过配置与 UI 开关启用，兼容旧配置。


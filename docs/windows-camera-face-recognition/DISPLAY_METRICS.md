# Windows 预览渲染：闪屏诊断与统计口径

## 1. 背景
本项目 Windows 预览从基于 GDI 的 `WM_PAINT + StretchDIBits` 路径升级为 `D3D11 + DXGI SwapChain`，目标是降低闪屏/撕裂，并为“分辨率/刷新率切换”和“72h 压测”提供稳定、可量化的指标输出。

## 2. 闪屏率 <0.1%：统计口径
本项目将“闪屏事件”定义为以下任一项发生（记为 1 次事件）：
- Present 失败：`IDXGISwapChain::Present` 返回失败（计入 `present_fail_count`）
- 设备丢失：Present 返回 `DXGI_ERROR_DEVICE_REMOVED` 或 `DXGI_ERROR_DEVICE_RESET`（计入 `device_removed_count`）
- SwapChain 重建：触发 SwapChain/后备缓冲重建（计入 `swapchain_recreate_count`）
- 黑帧输出：输入帧为空导致渲染输出黑帧（计入 `black_frame_count`，用于区分“渲染链路正常但采集异常”的情况）

闪屏率计算公式（百分比）：
- `flicker_rate_percent = 100 * (present_fail_count + device_removed_count + swapchain_recreate_count) / present_count`

说明：
- `black_frame_count` 不直接计入闪屏率，但必须记录，用于定位摄像头链路/识别线程导致的“画面黑/卡顿”。
- `swapchain_recreate_count` 通常出现在窗口尺寸变化或显示切换，短时间突增是预期行为；稳定运行中持续增长可视为异常信号。

## 3. “切换 ≤500ms”的计时点
切换耗时定义为：
- 起点：用户点击“应用显示”按钮（或稳定性测试模式触发切换）
- 终点：SwapChain 重新配置完成且完成一次成功 Present（即新配置下首次稳定输出）

实现口径：
- 以 `reconfigure()` + 随后一次 `renderFrame(nullptr)` 的 wall-clock 时间作为近似切换耗时（单位 ms）。

## 4. 闪屏根因盘点（升级前）
升级前的 Win32 预览路径包含以下典型闪屏诱因：
- `WM_TIMER -> InvalidateRect -> WM_PAINT` 的高频无效重绘；窗口消息调度抖动会放大重绘撕裂
- `hbrBackground != nullptr` + 默认 `WM_ERASEBKGND` 擦除导致背景闪烁
- GDI `StretchDIBits` 在高分辨率/高 DPI/多显示器下易出现拷贝抖动与刷新不一致

升级后的防护策略：
- 主窗口与预览窗口返回 `WM_ERASEBKGND = 1`，避免背景擦除闪烁
- 预览渲染从 `WM_PAINT` 改为定时 Present（SwapChain 双/三缓冲）
- 在 VSync off 时（且系统支持）启用 tearing 标志，降低同步导致的抖动

## 5. 指标输出文件
默认日志目录：`storage/win_logs/`（可通过 ini 的 `[log].log_dir` 修改）
- `render_metrics.jsonl`：每秒 1 条渲染统计（帧时间 P50/P95/P99、Present/设备丢失计数、RSS、句柄数、显存占用 best-effort）
- `event_log.jsonl`：稳定性测试/压测过程中的事件记录（切换成功/失败、耗时等）


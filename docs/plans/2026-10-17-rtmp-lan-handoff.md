<!-- DOCSYNC_DOC
type=plan
status=snapshot
as_of=2026-10-17
-->

# RK3288 局域网推流验证交接卡

**接续更新**：本快照的待续步骤已有后续执行，须先阅读[最新实测反馈](../audits/2026-10-17-rk3288-runtime-feedback.md)。`stable6`亦已取消，机B正式稳定段使用 `stable7`，稳定、重启和显式重试均已通过；不能直接照本卡旧步骤复跑或删除取消标记。旧结果与当时状态按快照保留。

更新：2026-10-17。完整验收未通过；本卡用于额度恢复或用户指定的接续会话。接续前确认工作区写入归属，不与其他会话同时修改。

## 1. 硬边界

- 不提交、不推送、不删除缓存，不覆盖现有工作区改动；既存未跟踪项 `pt` 不动。
- 两台保留数据覆盖安装已完成，不卸载主应用、不清数据、不改模型/模板/驱动/时间/权限。没有必要不要重新构建或安装主APK。
- 机器不得显示摄像头画面；仅使用临时控制器遮挡。严格像素守卫失败立即停止，不增加容差。
- 不保存或回传截图、录像、界面XML、原始敏感日志；截图仅在设备内存判断黑色，接收端只解码到空输出。
- 每段结束或失败均退出主应用、回到桌面、卸载临时控制器、删除本段防火墙规则。不得把测试心跳当成通过。
- 正式后台运行、正式隐藏预览只列待办，本次不实现。

## 2. 当前工作区与证据位置

仓库 `D:/19842/Documents/GitHub/rk3288_opencv`，分支 `codex/pr495-review-fixes`，交接前HEAD为 `43f0c6d`。

未提交文件：`cache/README.md`、`docs/guides/rk3288-device-acceptance.md`、`docs/index.md`、`docs/plans/2026-10-16-ffmpeg-kit-build.md`，以及新增的cache清单、局域网计划、遮挡设计和本卡。未修改生产应用源码。

所有临时实现及聚合证据在 `cache/rtmp-lan-2026-10-17/`，该目录被忽略，不能称为已入库或独立备份。必须保留既有失败结果，不覆盖结果文件。

固定主APK：`app-debug-real-core.apk`，65,740,689字节，SHA256：
`e6577d21e789e6d06409077e0f76b2c26fba7282e8ef26ef9d38cc2e3301a9db`。
真实OpenCV+ncnn双ABI构建、401项Java单测、lint和模型门禁通过；两台已装代码包摘要均与之相同，详见 `final-devices-A-B.json`。

当前控制器：`controller/build/outputs/apk/debug/RtmpTemporaryController-debug.apk`，SHA256：
`0aedc1cd977b20209b3b8d0af9fd592d9f632cb84496715f6194af9221837e3d`。
源码快照 `temporary-controller-source-04a3c8959594.zip`、APK副本 `temporary-controller-0aedc1cd977b.apk`，绑定见 `controller-evidence-source-binding.json`。取消标记不在快照内，不能解压覆盖当前目录。

消费AAR：`app/libs/ffmpeg-kit.aar`，SHA256：
`07dd0dcffbe52023b614e7455694093e9d17003626247f3ac9361b28aaf57eda`。
ARMv7 NEON/ARM64、x264/GPL、FFmpeg n6.0；备份在 `cache/ffmpeg-kit-build-2026-10-16/ffmpeg-kit.aar`。用途、恢复位置和许可边界见[构建记录](2026-10-16-ffmpeg-kit-build.md)。

## 3. 已完成与未通过

| 项目 | 机A | 机B |
|---|---|---|
| FFmpeg实际加载、30秒文件转推 | 通过，454帧 | 通过，454帧 |
| 相机稳定 | 305.3秒、3804帧，功能段通过 | 未通过，多次约10秒被像素守卫停止 |
| 正常停止/重启 | 首轮125→0帧失败；复验127→186帧通过，首轮原因未定 | 未执行 |
| 受控断流后显式重试 | 强化复验104→316帧通过，不等于自动重连 | 未执行 |
| 性能与内存 | 12.46fps低于15目标；PSS为0无效 | 仅文件段PSS 55,206KiB有效 |

机B先有366个非黑采样全部属于本应用Toast；透明处理Toast后仍有4个非Toast采样，独立黑窗也未解决。最新控制器发现并透明处理本进程StatusService状态悬浮窗（TYPE_PHONE/TYPE_APPLICATION_OVERLAY且包含 `tv_info`），无相机完整自检通过、发出 `OWN_STATUS_WINDOW_MASKED`；**尚未真实相机对照，不能宣称4点根因解决**。

用户确认无人操作、未见常驻提示；侧边截图时未见软件运行痕迹。测试段结束会退出桌面，因此该截图不能证明运行期间不存在提示或状态窗。

## 4. 提权迟到与安全状态

`cancel-B-camera-stable4`、`cancel-B-camera-stable5` 均已创建，严禁删除或复用旧标签。wrapper在创建规则前检查取消，Python在开始前、Instrumentation前及运行循环中也检查。

旧pwsh启动器PID13068已停止。第二次系统PowerShell调用返回子进程PID31932，用户随后确认已处理UAC；取消标记在确认前已写入。最后只读观察该PID已不在进程列表，但未生成 `admin-B-remaining-status.log` 或退出码文件，故不能推断其正常执行或退出码。不要重跑写死stable5的 `admin-b-remaining.ps1`。

自动审批因额度限制无法完成，“终止卡住的提权启动器”动作未执行；这不是安全拒绝，不得通过其他路径绕过审批。

最新 `handoff-safety-check.json`：两台主应用已退出、桌面可见、控制器包不存在；本机19350可绑定，两个取消标记存在。先前独立 `final-firewall-state.json` 为规则不存在；恢复前应再查规则，端口可用不等于规则不存在。

## 5. 接续顺序（先核对，再执行）

1. `git status --short`，保留全部改动；核对上述文件、固定包摘要、取消标记及两台退出状态。
2. 在有管理员令牌的本机PowerShell查询唯一规则 `RK3288-RTMP-Temporary-20261017-Codex`；若存在，先确认归属及是否仍有其他测试运行，不覆盖同名规则。
3. 仅机B，使用新标签 `stable6`。本机192.168.5.1，ADB A为192.168.5.2:5555、B为192.168.5.3:5555。此前批准的规则仅限两台来源、该本机地址TCP19350及已核验FFmpeg程序，每段finally删除。

```powershell
Set-Location 'D:/19842/Documents/GitHub/rk3288_opencv'
& './cache/rtmp-lan-2026-10-17/run-with-firewall.ps1' -Device B -Mode camera -Seconds 300 -Tag stable6
```

4. 确认接收帧、真实采集活性、严格像素守卫、时长、接收退出码和清理全部通过。若再现4点，立即保留失败，基于固定事件/聚合分区计数定位，不截图、不猜测、不放宽阈值。
5. 只有稳定段通过才逐项执行下面命令，每段非零立即停止，不连续粘贴批量运行：

```powershell
& './cache/rtmp-lan-2026-10-17/run-with-firewall.ps1' -Device B -Mode restart -Seconds 30 -Tag restart1
```

```powershell
& './cache/rtmp-lan-2026-10-17/run-with-firewall.ps1' -Device B -Mode retry -Seconds 45 -Tag guarded1
```

6. `run-controller.py`不允许覆盖已有结果；若标签已存在，先阅读该结果，再用新标签。每30秒主机心跳不是测试通过；重试首段主动杀接收器exit1为预期，最后一段仍须exit0且收到帧。
7. 最终运行 `python cache/rtmp-lan-2026-10-17/verify-final-devices.py`，检查双方代码包身份、进程退出、桌面、控制器和专用输入清理；另核对规则不存在、端口释放。
8. 修改 `summarize-validation.py` 的机B相机证据标签为实际完成的新标签，补齐机B证据变异，复跑汇总。现汇总完整验收为false，不能只改结果字段。机A首轮异常、吞吐不足、无效PSS和未做完整人脸精度验收继续保留。
9. 更新[局域网计划](2026-10-17-rtmp-lan-validation.md)，执行文档门禁与 `git diff --check`；代码有修改则 `graphify update .`。不得提交或推送。

## 6. cache整理边界

[cache清单](../audits/2026-10-17-cache-inventory.md)已分类可删候选与必须保留的源包、AAR、验证证据；**没有删除文件**。批量删除须另行报备内容、风险与回滚方案并等待确认，不能把“候选”当成已授权删除。

反思：功能段与完整验收必须分开报告；机B状态窗仅有自检证据。后续应先取得真实相机对照，再讨论根因闭环，避免被通过的合成测试误导。

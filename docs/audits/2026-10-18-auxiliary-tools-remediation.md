<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-10-18
-->

# 辅助工具核验整改

本整改轮已结束，以下测试数与变异结果属于该轮快照；当前工具使用见[审计工具](../guides/audit-tools.md)和[Android验证工具](../guides/android-validation-tools.md)，不自动继承为当前HEAD验收。

针对基线 `495f53f15e27466ae4cfe464f4a7fbad3b90316d` 的辅助工具审查，范围为 `scripts/`、相关测试及报告生成链。原始核验和本轮脱敏证据保存在本地忽略目录 `cache/reports/auxiliary-review-2026-10-18/`。本轮不连接设备、不启用相机或推流、不读取私有媒体，不清理真实资产，不推送或合并master。

| 问题 | 落地修改 | 对应验证 |
|---|---|---|
| F01 URL凭据泄漏 | 审计仪表盘的用户信息脱敏支持通用URI协议，包含RTMP/RTMPS/RTSP | 真实CLI合成输入检查HTML无账号、口令；副本退回HTTP限定表达式须失败 |
| F02 稳定性缺测仍通过 | 检查启动、HOME和日志读取失败，确认前后台有效组件；空日志与分区标题拒绝，黑屏判据失败返回非零 | 空日志、读失败、启动失败、无效焦点、黑屏及正常路径；缺测守卫变异 |
| F03 空基准计为样本 | PowerShell与Shell均等待TTFF、捕获成功/失败、服务重启四项完整非负整数，检查启动及查询失败 | 标题、部分指标、失败启动及有效指标；两种宿主脚本的判据变异 |
| F04 单设备标量崩溃 | PowerShell显式收集设备数组，基准日志同样固定数组语义 | 无设备、单设备、双设备、单行日志；移除数组包装的变异 |
| F05 恢复失败exit0 | 清理与恢复汇总失败数量并传播退出码1，保留逐项清单 | 缺失隔离源、合成文件正常恢复、注入隔离移动失败；禁用退出码传播变异 |
| F06 E2E清理文本断言不足 | 在既有契约测试之外运行真实PowerShell作业，通过替身提供回环HTTP及命令退出结果 | 成功、安装失败、E2E失败、覆盖率失败和服务未就绪五路径；注释清理命令变异 |
| F07 审计回归漏接CI | 用户批准后，将现有Windows pytest步骤改为 `python -m pytest tests/scripts/ -q`，保留精度工具环境与失败门禁 | YAML解析、实际测试收集；模拟恢复旧8文件清单须检出遗漏 |

复查另覆盖 `mResumedActivity: null` 出现在有效焦点记录之前的兼容路径：忽略空条目、继续寻找组件；全部焦点均无效时仍拒绝。基准目前要求应用输出既有约定中的全部指标；脚本不会替缺失的服务重启计数补0，也不会补造应用未实现的BENCH日志。

## 报告与兼容边界

稳定性输出不再保存原始 `logcat_full.txt`，改为 `logcat_summary.json` 的行数和判据计数，报告引用键为 `artifacts.logcat_summary`。原始系统日志可能带有其他应用的凭据或身份，不能因诊断便利直接落盘。旧历史报告不改写；依赖旧原始日志路径的外部人工流程须调整。

稳定性 `pass` 仅表示本轮焦点、采集和日志关键词判据通过，不证明实际画面无黑屏或冻结，也不是完整设备验收。焦点切到其他应用不等同于认证了某个特定桌面包；需要严格桌面身份时，使用已有 `scripts/android/verify-device-state.py --launcher-package ...`。本轮没有在真实设备运行这些有副作用的旧脚本。

E2E回归运行真实作业和本机5173回环端口，命令层由替身控制；不执行pnpm安装、Vite、Cypress浏览器或外网请求。夹具在断言前记录作业残留，即使副本变异破坏清理，也只回收自己创建的作业。前期夹具的阻塞监听问题已改为可取消轮询；超时运行不记作通过。

精度相关辅助测试使用已有 `build_fp15_gtest/bin/Release/` 工具和OpenCV公共级联模型处理合成输入，未重建C++二进制，未验证私有模型真实精度。本轮结果不能替代Gradle全量检查、产品构建或实际Cypress浏览器E2E。

## 复现

```powershell
# Git Bash、CMake、PowerShell和已有精度工具须可用；输出目录必须使用本轮新目录。
$env:RK_PRECISION_TOOLS_DIR = (Resolve-Path 'build_fp15_gtest/bin/Release').Path
$env:RK_PUBLIC_CASCADE_PATH = 'D:/ProgramData/OpenCV/opencv-4.10.0/data/lbpcascades/lbpcascade_frontalface.xml'
$auxCheckTmp = Join-Path cache/reports ('auxiliary-check-' + [guid]::NewGuid().ToString('N'))
python -m pytest tests/scripts/ -q --basetemp (Join-Path $auxCheckTmp 'fixtures')
node --test tests/scripts/*.test.js
```

上面路径是当前宿主的复现示例，其他宿主应显式指定其现有工具和公共模型；不能把缺少环境导致的跳过记为通过。Windows沙箱阻止Git Bash初始化或junction创建时，先记录环境失败，再在允许的宿主环境核验；不得删除历史证据目录来复用测试输出位置。

## 收尾状态

本轮7项问题均已落地。本地完整Node回归206项、Python回归172项全部通过，无遗留跳过。8组行为变异均由基线exit0、变异exit1确认检出；另有1项CI测试发现负向核验，恢复原8文件清单会被识别为遗漏审计及设备脚本回归。Python目录实际收集172项，其中审计工具36项、设备脚本22项。

文档门禁H0/M0/L10通过，低级提示未变。已运行 `graphify update .` 更新AST图谱；既有6份解析提示仍保留，未做语义重标记。原有报告与初次失败记录均未覆盖，最终执行目录、变异结果及发现核验分别见本地 `full-python-*`、`node-*`、`mutations-final.json`、`ci-discovery-verification-final.json`。

本地复查未发现上述整改范围内新的阻断项。远程CI、实际Cypress浏览器、产品全量重建及实机测试没有在本轮执行，不据此声明通过。分类提交前另查暂存区敏感信息；提交信息展示并获用户确认后才执行本地提交，禁止自行推送。

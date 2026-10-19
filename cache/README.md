<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-19
-->

# `cache/` —— 本项目本地可复用产物根目录

> **本目录的用途有版本记录（本文件入库，其余内容一律 gitignore）**，因此它**不是**「编译后未及时
> 清理的垃圾」：里面既有可重建产物，也有尚未入库的适配脚本与唯一验收证据；**不得整目录清理**。它是本项目约定的
> **可复用产物之家**。
>
> 判定标准很简单：**能提效复用的 → 放这里**；**一次性的 → 放 `D:\ProgramData\Temp`**。

## 分类

| 子目录 | 存放 | 典型内容 | 删除代价 |
|---|---|---|---|
| `builds/` | CMake / Gradle 构建目录 | `fp15_gtest/`（VS2022 + 真实 OpenCV 源码）等 | **高**（带真实 OpenCV 的配置是数 GB、重编耗时长）；`-DRK_SKIP_OPENCV=ON -G Ninja` 的快通道便宜 |
| `tools/` | 本机包装脚本 | `rk_android_gate.bat` / `rk_probe.bat` / `rk_docsgate.bat` / `rk_javatest.bat` | **低**（3–5 行，随时重建，见下） |
| `reports/` | 需**跨会话保留**的验证产物 | 基线哈希、门禁结论快照等 | 中（若未另存到记忆/计划文件） |
| `dependency-sources/` | 固定版本依赖源码 | OpenCV提交、GNU config固定提交 | 保留Git身份，未确认来源可恢复前不删 |
| `model-conversion/` | 模型转换中间件 | InsightFace三族PNNX/NCNN输出 | 保留来源、工具版本及路径映射，不能替代正式消费模型 |

正式FFmpegKit构建链已迁入[scripts/ffmpegkit](../scripts/ffmpegkit/README.md)，Android验证工具见[使用说明](../docs/guides/android-validation-tools.md)。本机旧工具原件待提交回读和消费者迁移完成后再逐项退出，不能按日期整删。

详细布局与恢复边界见[仓库存储布局](../docs/guides/repository-storage-layout.md)。当前AAR消费件仍为 `app/libs/ffmpeg-kit.aar`；新构建候选与历史实机消费件分开登记。

> ⚠️ **需要跨会话引用的结论（如基线哈希、审计裁定）应同时写进记忆或计划文件**，
> 不要只留在这里——缓存被忽略不等于其中的唯一证据可以删除。

## 重建方式

### 包装脚本（`tools/`）
每份只有 3–5 行，模板如下（`<仓库>` = `D:\19842\Documents\GitHub\rk3288_opencv`）：

```bat
@echo off
cd /d <仓库>
call "<仓库>\gradlew.bat" --no-daemon -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
exit /b %ERRORLEVEL%
```
- `rk_android_gate.bat`：上面这段（Android 全门禁：assemble + 单测 + lint）
- `rk_probe.bat`：同上去掉 assemble/lint，`--tests "%~1"`（单个全限定类名）
- `rk_javatest.bat`：同上，`--tests "com.example.rk3288_opencv.FaceModelManagerTest"`
- `rk_docsgate.bat`：门禁前置脚本（文档同步审计 + 仓库卫生扫描）。**必须存成 UTF-8 + CRLF**
  （本机 cmd 活动代码页为 65001；实测 UTF-8 注释若用 LF-only 换行，cmd 会误解析多字节注释并执行乱码、
  连带污染 `ERRORLEVEL` —— **必须 CRLF**）。⚠️ **两步都要跑到**（第一步失败不得跳过第二步，否则少一个
  信号）；`%ERRORLEVEL%` 必须先 `set` 存进 `AUDIT_EXIT` / `HYGIENE_EXIT` 再判（cmd 在 `if` 块里会提前
  展开 `%ERRORLEVEL%`，直接写 `if %ERRORLEVEL% neq 0` 会读到进入该块前的陈旧值）；**末尾必须以 `exit /b`
  透出真实退出码**：两步都为 `0` ⇒ `0`，否则先返回审计码（`AUDIT_EXIT`）、审计为 `0` 时才返回卫生码
  （`HYGIENE_EXIT`）。**末尾若缺 `exit /b`，最后一句是 `echo` 会让退出码恒为 `0`（假绿，本脚本曾犯此错）**：

  ```bat
  @echo off
  setlocal
  cd /d D:\19842\Documents\GitHub\rk3288_opencv
  node scripts\docs-sync-audit.js --skip-links --out-dir tests/reports/docs-sync-audit
  REM 先把 ERRORLEVEL 存进变量再判，避开 if 块里 %ERRORLEVEL% 的陈旧展开陷阱
  set AUDIT_EXIT=%ERRORLEVEL%
  echo AUDIT_EXIT=%AUDIT_EXIT%
  node scripts\clean-repo-junk.js scan --ci
  set HYGIENE_EXIT=%ERRORLEVEL%
  echo HYGIENE_EXIT=%HYGIENE_EXIT%
  REM 审计优先：审计为 0 时才返回卫生码
  if not "%AUDIT_EXIT%"=="0" exit /b %AUDIT_EXIT%
  exit /b %HYGIENE_EXIT%
  ```

### 构建目录（`builds/`）
```bash
# C++ 快通道（无 OpenCV，最快）
cmake -S . -B cache/builds/ci -G Ninja -DRK_SKIP_OPENCV=ON
# C++ 全量（需真实 OpenCV 源码目录；贵）
cmake -S . -B cache/builds/gtest -G "Visual Studio 17 2022" -A x64 \
  -DOPENCV_ROOT="D:/ProgramData/OpenCV/opencv-4.10.0" \
  -DOPENCV_CONTRIB_ROOT="D:/ProgramData/OpenCV/opencv_contrib-4.10.0"
```
> ⚠️ CMake 会把**绝对路径**烘进构建目录，故**移动已有构建目录会导致失效**，只能就地重建。

## 清理

先按[缓存盘点与保留清单](../docs/audits/2026-10-17-cache-inventory.md)区分测试临时件、昂贵构建、原始输入、适配脚本与验收证据。该清单是时点盘点，不是删除授权。

- 原始输入、唯一脚本、正式验收及变异证据先保留；需入库的文本复制到正式目录并脱敏核对后，再处理缓存副本。
- 临时夹具与编译缓存仅在使用它们的进程结束后列入删除批次；Cypress既有保留要求不因盘点解除。
- 批量删除前逐项报告绝对路径、逻辑大小、风险和恢复方法，取得确认后再执行；检查路径留在仓库缓存内，不跟随重解析点。
- 本目录有pytest链接、Windows路径安全测试链接等，不递归进入链接目标。逻辑文件大小不等于实际磁盘可回收空间。

## 相关约定

- 临时文件的落盘位置与跨目录访问规则见项目记忆 `feedback-file-paths`。
- 本目录**不参与**任何 CI 门禁；`cache/` 已在 `.gitignore` 中，仅 `README.md` 入库。

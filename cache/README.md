<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-09-19
-->

# `cache/` —— 本项目可复用内容的唯一根目录

> **本目录的用途有版本记录（本文件入库，其余内容一律 gitignore）**，因此它**不是**「编译后未及时
> 清理的垃圾」：里面的东西**可以随时整目录删除**（全部可重建），但只要它存在，就是本项目约定的
> **可复用产物之家**。
>
> 判定标准很简单：**能提效复用的 → 放这里**；**一次性的 → 放 `D:\ProgramData\Temp`**。

## 分类

| 子目录 | 存放 | 典型内容 | 删除代价 |
|---|---|---|---|
| `builds/` | CMake / Gradle 构建目录 | `fp15_gtest/`（VS2022 + 真实 OpenCV 源码）等 | **高**（带真实 OpenCV 的配置是数 GB、重编耗时长）；`-DRK_SKIP_OPENCV=ON -G Ninja` 的快通道便宜 |
| `tools/` | 本机包装脚本 | `rk_android_gate.bat` / `rk_probe.bat` / `rk_docsgate.bat` / `rk_javatest.bat` | **低**（3–5 行，随时重建，见下） |
| `reports/` | 需**跨会话保留**的验证产物 | 基线哈希、门禁结论快照等 | 中（若未另存到记忆/计划文件） |

> ⚠️ **需要跨会话引用的结论（如基线哈希、审计裁定）应同时写进记忆或计划文件**，
> 不要只留在这里——本目录允许被整体清理。

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

整目录可删：
```bash
rm -rf cache/builds cache/tools   # 或整个 cache/（README.md 可重新取回）
```

## 相关约定

- 临时文件的落盘位置与跨目录访问规则见项目记忆 `feedback-file-paths`。
- 本目录**不参与**任何 CI 门禁；`cache/` 已在 `.gitignore` 中，仅 `README.md` 入库。

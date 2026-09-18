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
- `rk_docsgate.bat`：`node scripts\docs-sync-audit.js --skip-links --out-dir tests/reports/docs-sync-audit`
  然后 `echo AUDIT_EXIT=%ERRORLEVEL%`，再 `node scripts\clean-repo-junk.js scan --ci` 后 `echo HYGIENE_EXIT=%ERRORLEVEL%`

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

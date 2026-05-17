# 推理节流配置（Windows）Task1 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows 配置体系中新增 `[inference]` 节流配置（INI 默认值 + C++ 结构 + JSON schema/序列化/解析），并更新 spec tasks.md 勾选 Task1。

**Architecture:** INI 作为首次迁移入口；首次运行会生成 `%APPDATA%\rk_wcfr\config.json` 作为唯一真源。为避免旧 JSON 因缺字段启动失败，JSON schema 将 inference 作为“可选字段”，解析阶段使用默认值兜底，后续落盘时写回完整字段。

**Tech Stack:** C++17、Windows INI API（GetPrivateProfileStringW/WritePrivateProfileStringW）、项目内 JsonLite/Schema 校验。

---

### Task 1: 更新 INI 默认配置

**Files:**
- Modify: [windows_camera_face_recognition.ini](file:///d:/19842/Documents/GitHub/rk3288_opencv/config/windows_camera_face_recognition.ini)

- [ ] Step 1: 新增 `[inference]` 默认值

将以下内容加入 INI（位置不重要，但建议放到 `[recognition]` 与 `[log]` 之间）：

```ini
[inference]
throttle_mode=auto
interval_ms=150
```

---

### Task 2: WinConfig 增加 inference 配置与 INI 读写

**Files:**
- Modify: [WinConfig.h](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/win/include/rk_win/WinConfig.h)
- Modify: [WinConfig.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/win/src/WinConfig.cpp)

- [ ] Step 1: 在 `WinConfig.h` 增加 `InferenceConfig` 并挂到 `AppConfig`
- [ ] Step 2: 在 `loadConfigFromIniOrDefault()` 中读取 `[inference] throttle_mode/interval_ms`
- [ ] Step 3: 在 `saveConfigToIni()` 中写回 `[inference] throttle_mode/interval_ms`
- [ ] Step 4: 规则
  - mode 仅接受 `auto|manual|off`，否则回退为 `auto`
  - interval 默认 150，并钳制到 80–500

---

### Task 3: WinJsonConfig 增加 inference（可选字段）并完成序列化/解析/校验

**Files:**
- Modify: [WinJsonConfig.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/win/src/WinJsonConfig.cpp)

- [ ] Step 1: 在 `schemaSettingsDoc()` 增加 `inference` object 到 `properties`，但不加入 root 的 `required`
- [ ] Step 2: 在 `toSettingsDocObject()` 增加 `inference: { throttleMode, intervalMs }`
- [ ] Step 3: 在 `parseAndValidateSettingsDoc()` 解析 `inference`，缺字段用默认值，写入时做同样的 mode 校验与 interval 钳制

---

### Task 4: 更新 spec tasks.md 勾选 Task1

**Files:**
- Modify: [tasks.md](file:///d:/19842/Documents/GitHub/rk3288_opencv/.trae/specs/fix-stutter-inference-throttle/tasks.md)

- [ ] Step 1: 将 Task1 中“定义自动模式调节参数”移动到 Task4
- [ ] Step 2: 勾选 Task1 与其两个子任务

---

### Task 5: 本地验证（尽力）

- [ ] Step 1: Windows 侧 CMake 配置（若环境具备 VS Build Tools）

在仓库根目录执行：

```powershell
cmake -S . -B build_win
```

期望：输出包含 “Configuring done/Generating done”。

- [ ] Step 2: 编译 Windows 目标（若上一步成功）

```powershell
cmake --build build_win --config Release --target win_local_service
```

期望：编译成功，无 C++ 编译错误。


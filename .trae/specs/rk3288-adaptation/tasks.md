# Tasks
- [x] Task 1: RK3288 硬件适配文档与约束
  - [x] SubTask 1.1: 调研 RK3288 芯片参数（CPU/GPU/内存带宽），编写 `docs/RK3288_CONSTRAINTS.md`。
  - [x] SubTask 1.2: 明确禁止 NPU API 调用，并在文档中声明 AI/推理方案（CPU NEON / GPU OpenCL）。

- [x] Task 2: 日志策略调整与声明
  - [x] SubTask 2.1: 检查并调整代码中的日志输出逻辑（Java/C++），确保 DEBUG/VERBOSE 包含详细信息，ERROR/WARN 保持简洁。
  - [x] SubTask 2.2: 在 README.md 中新增“日志免责声明”章节。

- [x] Task 3: 项目目录结构重构
  - [x] SubTask 3.1: 创建 `src/`, `scripts/`, `tests/`, `tools/` 等标准目录。
  - [x] SubTask 3.2: 将现有源码（`app/src/main/cpp`, `app/src/main/java` 等）按 `cpu/`, `gpu/`, `common/` 逻辑迁移至 `src/` 或在 `src/` 下建立软链/映射（需确保 Android 工程可编译）。注：若迁移成本过高，可考虑在 `src/` 下组织 Native 代码，Java 代码保留在 `app/src` 但在逻辑上归类，或调整 `build.gradle` sourceSets。
  - [x] SubTask 3.3: 迁移构建脚本至 `scripts/`，测试代码至 `tests/`，工具至 `tools/`。

- [x] Task 4: 文件清理与 Git 配置
  - [x] SubTask 4.1: 删除 `.tmp`, `.log`, `build/`, `.vscode/` 等冗余文件。
  - [x] SubTask 4.2: 创建/更新 `.gitignore`，排除非必要文件。

- [x] Task 5: 验证与报告输出
  - [x] SubTask 5.1: 验证编译流程（尝试使用新脚本或 Gradle 构建）。
  - [x] SubTask 5.2: 生成《项目整理报告》（Markdown），包含目录结构快照、文件统计、清理列表。

# Task Dependencies
- [Task 5] depends on [Task 1], [Task 2], [Task 3], [Task 4]

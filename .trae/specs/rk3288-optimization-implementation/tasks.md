# Tasks
- [x] Task 1: 安全配置加固
  - [x] SubTask 1.1: 修改 `app/src/main/AndroidManifest.xml`，设置 `android:allowBackup="false"`。

- [x] Task 2: 代码质量修复
  - [x] SubTask 2.1: 检查 `src/java/.../MainActivity.java`，定位并修复被忽略的 `NumberFormatException`，添加 `Log.w` 记录。
  - [x] SubTask 2.2: 检查 `scripts/build_android.bat`，将硬编码的 NDK/CMake 路径修改为从环境变量读取（如 `%ANDROID_NDK_HOME%`），并提供默认值或错误提示。

- [x] Task 3: 日志脱敏集成
  - [x] SubTask 3.1: 在 `src/java/.../AppLog.java` 中集成 `SensitiveDataUtil`。
  - [x] SubTask 3.2: 修改 `log` 方法，在写入磁盘前对 `msg` 进行脱敏处理（保留 DEBUG 模式下的详细输出选项，但在默认或生产配置下启用脱敏）。

- [x] Task 4: 编译配置确认
  - [x] SubTask 4.1: 读取 `CMakeLists.txt`，确认 `armeabi-v7a` ABI 下已包含 `-mfpu=neon` 等优化参数。
  - [ ] SubTask 4.1: 读取 `CMakeLists.txt`，确认 `armeabi-v7a` ABI 下已包含 `-mfpu=neon` 等优化参数。

# Task Dependencies
- [Task 3] depends on [Task 1]

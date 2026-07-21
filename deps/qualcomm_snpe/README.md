# deps/qualcomm_snpe

本目录是可选 Qualcomm SNPE（或相关 Qualcomm 推理 SDK）的本地依赖入口。仓库当前仅保留本说明文件，不随附 SDK 头文件、库文件，也没有可验证的 SDK 版本、来源或再分发许可记录。

## 当前检测与接入状态

- CMake 默认开启 `RK_ENABLE_QUALCOMM=ON`，仅在 `deps/qualcomm_snpe/include` 或 `$QUALCOMM_HOME/include` 下检测 `DlSystem/DlError.hpp`。
- 检测成功只会设置 `RK_HAVE_QUALCOMM=1` 并加入头文件目录；当前构建未查找或链接 SNPE 库。
- `Engine` 中的 Qualcomm delegate 仍是占位初始化，没有调用 SNPE API；因此“检测到头文件”不能视为推理后端可用。
- 缺少头文件时，CMake 配置阶段输出警告并保留 CPU 推理路径；运行期自检使用 `build_disabled` 或 `missing_dependency` 说明实际状态。

## 引入依赖时的记录要求

本地放置 SDK 前，应确认供应商许可允许当前使用方式，并同时记录 SDK 版本、获取来源、包校验值、支持平台和库文件位置。完成真实后端接入时，还必须补齐库查找/链接、SDK API 初始化与对应测试，不能只依赖头文件探测。

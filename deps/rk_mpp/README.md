# deps/rk_mpp

本目录是可选 RK MPP 的本地依赖入口。仓库当前仅保留本说明文件，不随附 MPP 头文件、库文件，也没有可验证的版本、来源或再分发许可记录。

## 当前检测与接入状态

- CMake 默认开启 `RK_ENABLE_MPP=ON`，在 `deps/rk_mpp/include`、`deps/rk_mpp/inc`、`$RK_MPP_HOME/include` 或 `$RK_MPP_HOME/inc` 下直接检测 `rk_mpi.h`；正确布局应为 `include/rk_mpi.h` 或 `inc/rk_mpi.h`，不是 `include/rockchip/rk_mpi.h`。
- MPP 实现还会包含 `mpp.h` 与 `mpp_err.h`，仅存在 `rk_mpi.h` 不能证明依赖完整。
- Android 目标在检测成功后显式链接 `mpp`；其他主机构建未配置通用的 MPP 库查找/链接，Windows 路径固定不启用 MPP。
- 非 Windows 平台的本地文件解码会优先尝试 `MppDecoder`，初始化或运行失败时保留 CPU/OpenCV 回退；运行期自检使用 `build_disabled`、`missing_dependency`、`runtime_init_failed` 或 `unsupported_platform` 说明实际状态。

## 引入依赖时的记录要求

本地放置 MPP 前，应同时记录版本、获取来源、包校验值、许可条件、目标 ABI，以及头文件和 `mpp` 库的对应关系。配置完成后需验证三个头文件、链接库和运行期 `mpp_create`/`mpp_init`，不能只以 `rk_mpi.h` 的存在作为可用性结论。

# deps/qualcomm_snpe

此目录用于存放 Qualcomm SNPE（或相关 Qualcomm 推理 SDK）的 headers/libs，以减少开发阶段重复编译与重复拉取依赖。

建议结构：
- include/...
- lib/<abi-or-platform>/...

构建行为：
- CMake 默认开启 Qualcomm 编译开关（`RK_ENABLE_QUALCOMM=ON`）。
- 若未检测到 `include/DlSystem/DlError.hpp`，将自动降级为占位实现（运行期回退 CPU），并输出 warning 日志。


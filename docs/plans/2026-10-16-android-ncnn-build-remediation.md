<!-- DOCSYNC_DOC
type=plan
status=snapshot
as_of=2026-10-18
-->

# Android ncnn 构建接线整改

本专项已结束，下述双ABI与回归结果保留为该轮快照；当前命令见[构建与测试](../guides/build-and-test.md#android)。完整APK或真机的未验收边界不因转快照而解除。

PR495 接续复核发现两项既有构建问题，用户已确认纳入修复，并批准应用 CMake/CI 补丁。本轮仅处理这组问题；Web/Cypress 由侧边会话处理。

## 根因与修复

- Gradle 提供 `NCNN_DIR_ARMv7/ARM64`，但 CMake 仅将其赋给 `NCNN_DIR`，没有在 `find_package(ncnn)` 时使用。正式配置现在搜索当前 ABI 安装根，并避免 NDK sysroot 重定向；仍支持显式 `ncnn_DIR` 与缺依赖回落。
- 官方 Android 预编译包将 `-fno-rtti/-fno-exceptions` 导出给消费者，导致项目和 OpenCV 头文件中的异常处理及 RTTI 编译失败。仅移除 Android 消费方的这两个选项，保留其他选项，兼容普通、命名空间及别名 target；不修改外部包或 ncnn 自身的构建选项。
- 新增 `tests/scripts/test_ncnn_cmake_behavior.py` 的11项行为回归，并接入现有 Windows CI 精度工具 pytest 命令。

## 正式配置验证

先在未修改的生产模块复现：11项回归中6项失败、5项通过。候选隔离验证通过后，才按用户批准应用补丁。

应用后移除候选模块替换与手工 `ncnn_DIR`，重新执行：

| 检查 | 结果 |
|---|---|
| CI 同款精度工具与 CMake 组合回归 | 23项通过，无跳过 |
| 双 ABI 当前 `native-lib` 全量对象编译 | 100/100通过；每 ABI 50项，全部 `RK_HAVE_NCNN=1` |
| 独立 JNI 链接 | 2/2通过，复用既有同 ABI OpenCV 缓存，未重编 OpenCV |
| 精准变异 | 4组均检出，每组3项失败、8项通过 |
| 编译期间源码与项目头文件漂移 | 0 |

变异分别恢复安装根丢失、禁用 RTTI、禁用异常，以及误删全部接口选项；均由预期断言检出，未以配置异常或超时充当检出。

本机证据位于 `cache/audit-fixes-2026-10-13/nonweb-followup-2026-10-16/`，包括 `production-tests.log`、`production-configure-summary.json`、`production-objects/` 和 `production-mutations/`。缓存不入库；保留原34项编译失败记录与候选记录。

## 验收边界与回滚

以上证明正式配置的对象编译及复用缓存的独立链接通过，**不等于完整 Gradle/APK 构建、设备运行、真实人脸精度或推流验收**。本轮不推送，不修改侧边 Web 文件及共享图谱。

真正启用 ncnn 可能增加链接体积并改变后端可用状态。回滚时撤销 `cmake/opencv.cmake` 和 CI 命令的对应改动；新增测试可保留用于复现缺陷，也可随修复撤销。

反思：陈旧编译清单的局部成功掩盖了后端禁用与接口选项传播；验证应绑定当前目标清单，先确认有效后端宏，再区分编译、链接和设备验收。

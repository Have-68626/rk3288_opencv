# BSP 证据登记与 Release Note 状态

## 当前结论

仓库尚未保存厂商 BSP Release Note、交付包标识或原始 BSP defconfig，因此不能据此声明“最新 BSP 版本”、Android 版本、厂商补丁集或驱动变更历史。当前只能确认仓库内运行态内核配置快照所表达的配置事实。

## 已核验的本地证据

- `kernel-config/kernel.config` 文件头标识为 `Linux/arm 4.4.143 Kernel Configuration`，并包含 `CONFIG_ARCH_ROCKCHIP=y` 与 `CONFIG_CPU_RK3288=y`。
- 相机/USB 相关配置包含 `CONFIG_USB_VIDEO_CLASS=y`、`CONFIG_USB_EHCI_HCD=y`、`CONFIG_USB_OHCI_HCD=y` 和 `CONFIG_USB_STORAGE=y`。
- 显示、编解码与音频相关配置包含 `CONFIG_DRM_ROCKCHIP=y`、`CONFIG_ROCKCHIP_RGA=y`、`CONFIG_ROCKCHIP_RGA2=y`、`CONFIG_RK_VCODEC=y` 和 `CONFIG_SND_SOC_ROCKCHIP=y`。
- 快照同时显示 `CONFIG_VIDEO_ROCKCHIP_CIF`、`CONFIG_VIDEO_ROCKCHIP_ISP1` 与 `CONFIG_ROCKCHIP_MPP_SERVICE` 未启用。配置项只能证明构建选择，不能证明设备驱动已完成真机功能或稳定性验证。
- `defconfig/rk3288_defconfig` 是从运行态快照提取的 44 项审计子集，当前逐项与快照一致；它不是厂商原始 defconfig。

当前证据完整性校验：

```text
SHA256(kernel-config/kernel.config) = 0299DE190ADA418899AE6B71C54B39C3447863EF45B96214E0CFEFFA69DE9545
SHA256(defconfig/rk3288_defconfig)    = D6213B09630E78CB7ECCB066F02B22AC2C926B7587E7FF2869DCBA31A030328F
```

## 尚缺证据与采集要求

补齐厂商材料时，必须记录 BSP 名称/版本、供应商交付编号或包名、原始 Release Note 的保存位置与 SHA-256、原始 defconfig 路径，以及脱敏后的 `uname -a`、`/proc/version` 和 Android 版本信息。若材料不能入库，只记录受控存储位置和校验值，不复制设备序列号、账号或内部地址。

在取得上述证据前，相机、USB、存储、显示、音频、网络和 72 小时稳定性结论应以专项真机测试记录为准；不得从当前配置快照反推厂商修复项或已知缺陷状态。

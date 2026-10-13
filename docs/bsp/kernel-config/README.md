# Kernel config 快照目录

本目录保存从目标设备导出的运行态内核配置快照。当前 `kernel.config` 的文件头标识为 `Linux/arm 4.4.143 Kernel Configuration`，包含 `CONFIG_ARCH_ROCKCHIP=y`、`CONFIG_CPU_RK3288=y`、`CONFIG_IKCONFIG=y` 与 `CONFIG_IKCONFIG_PROC=y`。

`../defconfig/rk3288_defconfig` 是从该快照提取的 44 项文档审计子集，当前逐项匹配；它不是厂商 BSP 源码中的原始 defconfig。运行态配置也不能单独证明设备型号、Android 构建、驱动补丁来源或硬件功能已验收。

## 当前快照校验值

```text
SHA256(kernel.config)                    = 0299DE190ADA418899AE6B71C54B39C3447863EF45B96214E0CFEFFA69DE9545
SHA256(../defconfig/rk3288_defconfig)    = D6213B09630E78CB7ECCB066F02B22AC2C926B7587E7FF2869DCBA31A030328F
```

## 重新导出

设备允许 root 且提供 `/proc/config.gz` 时：

```bash
adb root
adb shell ls -l /proc/config.gz
adb shell "zcat /proc/config.gz > /data/local/tmp/kernel.config || gzip -d -c /proc/config.gz > /data/local/tmp/kernel.config"
adb pull /data/local/tmp/kernel.config docs/bsp/kernel-config/kernel.config
```

导出后必须确认文件头、`CONFIG_CPU_RK3288=y` 和关键配置项，重新生成审计子集并核对每项值，再同步更新本文件与 `../BSP_RELEASE_NOTES.md` 中的 SHA-256。采集记录应脱敏，不保存设备序列号、账号或内部地址；若 `/proc/config.gz` 不存在，应从对应厂商 BSP 构建产物获取 `.config`，并明确标注来源而不是伪装成运行态快照。

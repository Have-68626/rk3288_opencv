# Kernel config 快照目录

本目录用于存放从设备导出的“运行态内核配置快照”（推荐来源：`/proc/config.gz`）。

## 推荐文件名
- 最新快照（固定名，供审计脚本默认读取）：`rk3288_kernel.config`

## 导出方法（设备支持 root 时）
```bash
adb root
adb shell ls -l /proc/config.gz
adb shell "zcat /proc/config.gz > /data/local/tmp/kernel.config || gzip -dc /proc/config.gz > /data/local/tmp/kernel.config"
adb pull /data/local/tmp/kernel.config docs/bsp/kernel-config/rk3288_kernel.config
```


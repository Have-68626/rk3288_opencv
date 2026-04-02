# BSP Release Notes（占位）

本文件用于固化“目标 RK3288 BSP 最新 Release Note”的关键内容，作为文档同步审计与缺陷归因依据。

请在拿到厂商 BSP Release Note 后，将以下信息补齐并维护：

## 1. 基本信息
- BSP 名称/版本号：
- 获取方式（URL/包名/供应商交付编号）：
- 对应内核版本（`uname -a` / `cat /proc/version`）：
- 对应 Android 版本（`getprop ro.build.version.release`）：
- Release Note 原文存放位置（若可放入仓库请给出路径；否则给出外链与 hash）：

## 2. 内核配置/defconfig 变更摘要
- 关键 CONFIG_ 变更（启用/禁用/改为模块）：
- 影响面：相机、USB、存储、显示、音频、网络等

## 3. 驱动与补丁摘要
- Camera/ISP/UVC：
- USB（HOST/HUB/存储）：
- Display/HDMI：
- Audio：
- Ethernet：

## 4. 已知缺陷（必须与 docs/RK3288_CONSTRAINTS.md 对齐）
- 相机相关：
- USB 相关：
- 存储相关：
- 长时间运行（72h）相关：


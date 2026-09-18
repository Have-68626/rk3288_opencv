<!-- DOCSYNC_DOC
type=design
status=living
as_of=2026-09-18
-->

# RK3288 指纹 / 读卡外设接入设计（未落地）

本文是**设计稿**，不是实现记录。**项目中没有任何一行指纹或读卡接入的实现代码**：没有 root helper、
没有 `SG_IO` 客户端、没有与既有加密存储对接的调用方。除明确标注为"已存在"的既存组件外，本文所述
一切均为**待实现、未验证**的设计意图。不得据此宣称任何能力"已落地""已实现""已验证"。

## 1. 背景与本文定位

设备上的 `/dev/sg0`（SCSI INQUIRY 自报 `WINUIM` / `WMR06`）是**已确认存在、且项目能力清单已记录**
的外设；但此前的文档只把它列为"存在的外设节点"，**从未给出接入设计**。具体缺口：

- **root helper 如何实现**——`/dev/sg0` 权限为 `crw------- root root`，普通应用不可访问，接入必须
  经 root 通道；该通道的形态、生命周期与权限模型，此前**全无文档**。
- **权限模型**——应用沙箱（app uid）与 `uid=0(root)` 之间的边界如何划定，无文档。
- **与既有加密存储的关系**——既存 `FeatureTemplateEncryptedStore`（AES-GCM 信封）是否复用、复用点
  在哪、谁负责落盘，无文档。

本文的定位是**补齐这份设计**：说明"是什么 / 为什么这样设计"，并**逐项标注未核实项**。设备画像的
细节不在本文重述，统一引用单一事实源：`docs/research/rk3288-device-capability-survey-2026-09-17.md`
与 `../RK3288_CONSTRAINTS.md`。本文只描述与该外设接入直接相关的那一小部分设备事实。

## 2. 设备事实基线

本节事实以 **2026-09-18 一轮只读取证** 为准。该轮取证的事实集是**本机临时件、不入库**，故本节
**不再以任何小节号引用它**（以下简称「**本轮取证**」）；每条事实**就地写全**，或改引**仓库内**出处——
落地结论另见 `docs/RK3288_CONSTRAINTS.md` §0.4 与 `docs/research/rk3288-device-capability-survey-2026-09-17.md`。
**不自行推算设备行为**；取证范围之外的数字一律视为未取证。

### 2.1 设备节点与权限

- 设备节点 `/dev/sg0`，设备号 `21,0`，权限 `crw------- root root`（本轮取证）。**两台设备均如此。**
- 推论（设计约束）：只有 `uid=0` 可打开该节点。**普通应用（app uid）不可直接访问，接入必须经 root
  通道**——这是本设计成立的前提。
- SELinux 为 `permissive`：`ro.boot.selinux=permissive`、`androidboot.selinux=permissive`（本轮取证）。
  `adb shell` 默认即 `uid=0(root)`、`context=u:r:su:s0`（本轮取证）。
  ⚠️ **permissive 下可访问 ≠ enforcing 下可访问**：`/dev/sg0` 的 SELinux 上下文**未取证**，本设计
  不得据此断言在 enforcing 策略下同样可访问。

### 2.2 USB 侧与接口身份

- USB 设备：`22bc:2009` `USBKey Module`，`manufacturer` = `USBKey Chip`（本轮取证）。
- SCSI INQUIRY：`vendor=WINUIM` / `model=WMR06` / `type=5` / `rev=0110`；**两台一致**（本轮取证）。
- 接口形态：`bInterfaceClass=08`（Mass Storage）/ `SubClass=06`（SCSI）/ `Protocol=50`（Bulk-Only）；
  `bcdUSB=0x0110`（USB 1.1）；两个 64 字节 bulk 端点（`0x81` IN / `0x01` OUT，`wMaxPacketSize=0x0040`）；
  **无中断端点**；`bMaxPower=0x64`（200mA）。接口 `DRIVER=usb-storage`（本轮取证）。

### 2.3 根本约束：只剩 SCSI generic 一条路

- **内核无 `sd_mod`** → `usb-storage` 绑定后**不产出任何块设备**（`/sys/class/block/` 无 `sd*`）→
  要读卡只剩 `/dev/sg0` 这一个 SCSI generic 节点（本轮取证）。
- **这是本设计的根本约束：接入只能走 SCSI generic（`SG_IO`）**，不存在"当 U 盘挂载后读写文件"的
  退路。
- 机 B 内核级硬证据：`/sys/class/scsi_device/0:0:0:0/device/` 的 `state=running`、`iorequest_cnt=0x1`、
  `iodone_cnt=0x1`、`ioerr_cnt=0x0` → **自开机以来该 SCSI 设备只收到过 1 条请求**（枚举时的
  INQUIRY），之后零流量（本轮取证）。

### 2.4 当前无使用者

- **两台均无任何进程持有** `/dev/sg0`（已做有阳性对照的 fd 扫描；机 A 阴性对照时 `pids=192`、
  `fdlinks=1599`，确认循环确实跑到）（本轮取证）。
- 厂商组件进程（`com.adtv`、`com.ys.ys_receiver`、`com.yx.factorytest`、`com.example.yxset` 等）
  **全部不持有任何串口 / sg / hidraw 节点**（本轮取证）。
- `/init.rc`、`/init.rk30board.rc`、`/init.usb.rc`、`/ueventd.rc` 等**均无**对 `sg0` / `usbkey` /
  `WMR06` 的引用（本轮取证）。

### 2.5 事后侧载历史

- `winuim.fingerprint.sample` 与 `com.genee.hw_test` 装在 `/data/app/`，时间戳 `2021-08-25`，
  **晚于 ROM 构建 `2021-08-05`** → 机器**事后被侧载过**指纹相关测试程序（本轮取证）。
- 厂商 APK 字符串扫描（机 B，关键词 `wiegand|cardno|card_no|卡号|读卡|rfid|mifare|13.56|nfc` 等）
  **6 个 APK 全部 0 命中**，且管线自证（`unzip -p … classes.dex | wc -c` 与 `unzip -l` 的 dex 大小一致）
  排除了假阴性（本轮取证）。

### 2.6 设备 ID 去标识口径

设备一律用 `device_id_hash` 标识（SHA-256 前 12 位小写 hex，口径同 `scripts/run-android-face-regression.ps1`
的 `Get-Sha256Short`，去标识见 `docs/research/rk3288-device-capability-survey-2026-09-17.md` §7.1）：机 A `69d83c2ce659`、
机 B `f09acaca146d`。**本文及后续文档不得出现明文序列号。**

## 3. 身份认定与未定论项（设计前必须声明）

本节刻意放在设计正文之前。**§3.1 给出已认定的设备身份及其依据**；**§3.3 列出本轮仍未定论、设计不得当成已解决的项**。

### 3.1 `WINUIM WMR06` 的身份：指纹仪（另有"是否兼具读卡"未验证）

**身份为指纹仪，有直接证据**（2026-09-18 修订：本节此前写作"指纹仪还是读卡器未定论"，属**概念错误**——它把本设备与**另一个独立设备**（读卡器）混为一谈）：

- 2026-09-17 机 B 经厂商示例应用**端到端采到带脊线的指纹图像与模板**（`docs/research/rk3288-device-capability-survey-2026-09-17.md` §2.3）——**读卡器不可能产出指纹图像**；
- 厂商 SDK 为 **Futronic 系**（`libftrScanAPI.so`；错误串含 `CFs80CompatibleDevice`、`CFsUsb20t80HDevice`）；
- 厂商自带示例应用即名为指纹示例（`winuim.fingerprint.sample`）；
- 且**指位与读卡区在物理上是分开的**：指纹在机身底部**最右侧**，读卡区在底部**最左侧**——两者**不是同一设备**。

**接口形态与身份不矛盾**：`/dev/sg0` 声明为 Mass Storage / Bulk-Only 外壳（`08/06/50`）、因缺 `sd_mod` 而无块设备、自开机仅 1 条 SCSI 请求（机 B）、当前无进程持有——**Futronic 系指纹仪本就以待 SCSI 设备的形态出现**，这正是"声明为大容量存储却不产出块设备"的原因。

**仍未验证**：① 它**是否兼具读卡功能**；② **两台是否都可用**（端到端仅机 B 测过）；③ 其私有 SCSI 命令集**未逆向**（需 `SG_IO`，属"向硬件发命令"，须单独授权）。

### 3.2 本设计的主线：指纹模板流

- **主线（§5）**：helper 取回指纹图像 / 特征字节，应用侧加密为"模板"落盘——与 §3.1 已认定的指纹仪身份一致。
- **条件分支**：**若**后续验证发现它**兼具读卡功能**，则 §5 需补一条"卡号 / 低熵标识"的存储路径（卡号是低熵标识，与"图像 / 特征向量"的加密语义不同，AAD 域与文件名派生策略须另定）。

下文在需要处用"模板 / 数据"并列表述，即为给这条条件分支留位置。

### 3.3 其他在本文档范围内保留为未定论的项

- `c0f4:06f5` 是否为读卡器的"键盘楔（keyboard wedge）"化身——**未验证**（本轮取证）。
- 该外设的宿主接口（除 `SG_IO` 外是否还有别的通路）——**仍未确定**，已排除项见 `docs/research/rk3288-device-capability-survey-2026-09-17.md` §6。

## 4. 接入路径候选与取舍

`/dev/sg0` 只对 root 开放（§2.1），因此三条候选路线都必须先回答同一个问题：**root 权限从哪来、
以什么形态存在、生命周期多长。**

### 4.1 候选 A：root helper 进程

由项目自建一个以 root 运行的进程，负责打开 `/dev/sg0` 并执行 `SG_IO`。两种形态：

**形态 A1 —— 一次性 `su -c` 调用**

- 应用每次需要设备 I/O 时，拉起一个短命 root 进程（native 二进制），完成一次采集后退出。
- 权限模型：每次调用都重新跨越 app uid → root 边界；root 授权由 ROM 的 `su` 策略决定。
- 生命周期：与单次采集同生共死，无常驻攻击面。
- 与沙箱关系：helper 进程**不在**应用沙箱内，只通过标准输入输出 / 临时文件交换**原始字节**；数据
  落盘接口留在 app 侧（呼应 §5 的硬边界）。

**形态 A2 —— 常驻守护 + 本地 socket（`AF_UNIX`）或 AIDL**

- 开机后由 root 启动一个守护进程，应用经 `AF_UNIX`（或 AIDL binder）请求采集。
- 权限模型：**须由 helper 自身对 peer 做鉴权**——`AF_UNIX` 需校验 peer uid / 使用 `SO_PEERCRED`；
  AIDL 若由普通 app 提供 Service，则**该 Service 本身就运行在 app uid 而非 root**，无法直接打开
  `/dev/sg0`（这一点决定了 AIDL 只能作为"应用侧接口"，真正的 root 部分仍需 A1 式拉起或 ROM 内置）。
- 生命周期：常驻 → 请求延迟低，但**长期暴露一个 root 端点**，是更大的攻击面。
- 与沙箱关系：跨边界更频繁，鉴权与协议校验的成本更高。

> **未验证前提**：本 ROM 上**应用进程能否调起 `su`**（而非仅 `adb shell` 默认 root）**未取证**。
> 本轮取证只记录了 `adb shell` 侧 `uid=0(root)`，未记录 app uid 侧的 `su` 可用性。
> 该前提不成立时，候选 A 的两种形态都无法落地，只能走 ROM 内置通道。

⚠️ **本 ROM 的固有风险面（本轮取证）**：`yx_daemon.sh` 的 `get_yxadb()` 会
`find /mnt/media_rw/ -name yx_adb.sh` → 命中即 `cp` 到 `/system/bin/yx_adb.sh` → **`exec` 之**。即
**插入含 `yx_adb.sh` 的 U 盘即可让设备以 root 执行该脚本**。这是 ROM 的**固有问题（非本设计引入）**，
但它意味着：任何自建 root 通道都必须假设"设备上已有其它 root 执行面"，**不能把 root helper 当作
可信边界的唯一定义者**。

### 4.2 候选 B：复用厂商 SDK

任务交接口称 `winuim.fingerprint.sample` 携带以下预编译库：`libJNIFp_Sdk_WMR08.so`、
`libftrScanAPI.so`、`libfp_sdk.so`、`libscsiusb.so`、`libusb-1.0.so`、`libserial_port.so`。
（**该库清单来自任务交接；本轮取证只记录了该 APK 的存在与侧载时间，未记录其内部库
清单**——库名与路径**未核实**。）

- **这些库的许可证与来源均未核实。** 侧载 APK 非本仓库产物，来源不明；无许可证声明即默认"保留
  所有权利"，**不可用于产品**（与本项目 `docs/research/opensource-integration-feasibility-2026-09-16.md`
  对 `mjpg-streamer` 的既有判断同一口径）。
- 全部为**预编译 `.so`**，属**供应链风险**：无法审计、无法重编、ABI / 平台绑定、可能夹带未声明行为。
- "复用"与"自研"的代价差异：复用看似省去逆向工作量，但代价是把不可审计的二进制纳入产品，且**仍然
  需要 root 通道**（这些库同样要打开 `/dev/sg0`）。**复用不能免除候选 A 的权限模型问题**。
- 本轮取证的字符串扫描对该 APK 的 6 个关键词**全 0 命中**——即**没有**从 APK 字符串层面拿到身份证据。
  **但这不影响 §3.1 的结论**：身份是由"端到端采到指纹图像"这一**行为证据**认定的，不是由 APK 字符串认定的。

### 4.3 候选 C：自研 `SG_IO` 客户端

由项目自行实现一个最小 SCSI generic 客户端，直接对 `/dev/sg0` 下发命令。

- **需要逆向 SCSI 命令集**：厂商私有的采集命令（opcode、CDB 布局、返回数据格式）**未逆向**。这是
  主要工作量与主要未知量。
- 需要 root 通道（同候选 A）。
- 风险：在命令集不明时，"试"命令本身就是**向硬件发命令**（须单独授权），且错误命令可能改变设备
  状态。**本文不发送任何命令。**
- **为何它是唯一"拥有代码"的路线**：候选 A 解决"谁来跑"，候选 B 是黑盒二进制；只有候选 C 让项目
  真正**拥有**与设备对话的代码（可审计、可重建、可写中文注释、可纳入测试）。这与本项目对
  "拥有代码 vs 引入不可审计依赖"的既有取向一致。

### 4.4 推荐路线与依赖的未验证前提

**推荐：候选 A（root helper，形态先 A1 后视需要评估 A2）承载候选 C（自研 `SG_IO` 客户端），不采用
候选 B。**

理由：A + C 是唯一"拥有代码 + 通道可控 + 无供应链风险"的组合；B 的许可证 / 来源 / 可审计性均不达标，
且并不能免除 root 通道本身的设计问题。

**推荐路线明确依赖以下未验证前提（任一不成立则需回退重估）：**

1. **该设备确可被 `SG_IO` 访问且命令集可逆向**——未验证；需先取得"向硬件发命令"的单独授权。
2. **本 ROM 上应用侧 root 通道可用**（app uid 能经受控方式获得 root，或存在可用的 ROM 内置通道）——
   未取证。
3. **enforcing SELinux 下 `/dev/sg0` 可访问**——未取证（当前为 permissive，§2.1）。
4. **该设备确为指纹仪**——**已认定**（§3.1：端到端指纹图像 + Futronic 系 SDK + 指位与读卡区物理分开）。**尚未验证的是它是否兼具读卡功能**；若兼具，§5 须补低熵标识的存储路径。
5. **机 B 的 `/data` 余量足以承载落盘**——机 B 仅余 624M（见 `docs/research/rk3288-device-capability-survey-2026-09-17.md` §7.1 与 `docs/RK3288_CONSTRAINTS.md` §3.3），**是现实约束**（§7）。

## 5. 模板与数据的存储设计

> 前提（§3.2）：本节主线按**指纹模板**设计。若后续验证发现该设备**兼具读卡功能**，本节需补"卡号 / 低熵标识"的存储路径（复用点须重新评估）。

### 5.1 与既有 `FeatureTemplateEncryptedStore` 的关系

先读既有实现 `src/java/com/example/rk3288_opencv/FeatureTemplateEncryptedStore.java` 以对齐设计
（该文件已存在；本文只引用，不重述其接口细节）。其关键事实：

- **AES-GCM 信封**：魔数 `FST0`、`VERSION=1`、IV 长 12、GCM tag 128 位。
- **AAD 域**：模板的 AAD = `sha256(userId) ‖ modelVersion(u32 BE) ‖ templateSchemaVersion(u32 BE)`；
  显示名走**独立域** `NAM0`（`NAME_AAD_DOMAIN`）+ `sha256(personId)`，跨用途换用会被 GCM tag 拒绝。
- **文件命名**：模板 `u_<sha256(userId)>.bin`；显示名 `n_<sha256(personId)>.enc`。文件名单向派生，
  不落明文标识。
- **`PlatformKeyProvider` 接缝**：生产走 AndroidKeyStore（API ≥ 23）；测试可注入 JCE 内存密钥。
- **降级语义**：平台不支持时返回 `UNSUPPORTED_PLATFORM`，**绝不回落明文**。
- **落盘语义**：临时文件 + flush + fsync → `dst→bak` → `tmp→dst` → 清 `bak`，原子替换，`.bak` 可恢复。

**本设计对它的复用关系（设计意图，未实现）：**

- 指纹**模板（特征字节）**落盘**复用同一套信封与原子落盘语义**——即若为指纹仪，指纹模板与非指纹
  人脸模板可共用 `FeatureTemplateEncryptedStore` 的信封格式与崩溃恢复路径。
- **但 AAD 域须新增**：指纹模板与既有人脸模板、显示名**不能同域**。设计上应引入一个**新的域标签**
  （类比既有的 `NAM0`），使"把指纹模板文件喂给人脸模板读取路径"这类换用**因 GCM tag 校验失败而被
  拒**，而不是静默换用。
- 若设备实为读卡器，则"卡号"是**低熵标识**，与"高熵特征向量"的威胁模型不同：文件名派生 / AAD 域
  的设计需要独立评估（低熵 personId / 卡号可被字典反查），**不得直接照搬人脸模板的域设计**。

### 5.2 硬边界：root helper 只做设备 I/O，不负责落盘明文

这是本设计最重要的一条边界，用来隔离"持有硬件权限的进程"与"持有加密密钥的应用"：

```text
[设备 /dev/sg0]
      |  SG_IO（root）
      v
[root helper]  —— 只做设备 I/O；不写任何明文文件；不接触 Keystore
      |  返回原始字节（指纹图像 / 特征字节，或卡号）
      v
[应用侧 app uid]
      |  FeatureTemplateEncryptedStore.write(...)（Keystore AES-GCM）
      v
[files/face_templates/ 或新增目录]  —— 只落密文信封
```

- **helper 不落盘明文**：它返回的是**原始图像或特征字节**（或卡号），**加密落盘由应用侧完成**。
- **helper 不接触密钥**：Keystore 密钥只在 app uid 侧使用，root helper 无从解密。
- **边界清晰**：helper 的职责被限定为"打开节点 → 下发命令 → 回传字节"；任何"落盘 / 命名 / 域选择"
  逻辑都留在应用侧。这与本项目既有的"算法关键逻辑 / 硬件差异处理写中文注释"约定一致，也便于将来
  对 helper 做最小化审计。

### 5.3 降级不得回落明文

沿用既有语义（与 `UNSUPPORTED_PLATFORM` 一致）：

- helper 不可用 / 设备打开失败 / `SG_IO` 失败 → **显式报错（受控失败码）**，**不得**改为把原始图像
  或特征以明文落盘。
- Keystore 不可用（API < 23 或密钥损坏）→ 返回 `UNSUPPORTED_PLATFORM` / `KEYSTORE_ERROR`，**不新建
  明文文件**。
- 临时采集帧 / 图像**仅在内存中存活**，使用后清零，**不落盘**（对齐既有人脸注册"不存照片、不存原始帧"）。

## 6. 验收项接口（草案）

以下是一组**待实现的验收判据草案**，供 `../guides/rk3288-device-acceptance.md` 后续引用。
**明确声明：这些是草案，尚未加入验收清单；未执行、未通过。** 编号沿用该清单的字母体系，前缀
`E` 表示新增的外设接入组（命名待定，最终以验收清单为准）。

| 编号 | 判据草案 | 说明 |
|---|---|---|
| E1 | 设备可打开 | root helper 能以只读方式 `open("/dev/sg0")` 成功，并显式报告成功 / 失败原因 |
| E2 | SN 可读 | 能读到设备标识（INQUIRY 或厂商 SN 字段）；**读取若会下发 SCSI 命令，须先取得授权** |
| E3 | 采集成功 | 一次采集返回非空的图像 / 特征字节，且长度与格式符合约定 |
| E4 | 模板落盘为密文 | 落盘文件为 `FST0` 信封密文，可被对应 AAD 域解出 |
| E5 | 无明文图像落盘 | 采集过程中**无任何原始图像 / 特征明文落盘**（检查应用私有目录与临时目录） |
| E6 | helper 不可用时显式报错 | helper 缺失 / 无 root / 设备打开失败时，**显式返回错误码**，**不得静默降级**为明文或空成功 |

补充约束（同样属草案）：

- **E5 须在 `adb logcat` 与文件系统两侧核对**：日志不得含模板 / 特征 / 明文标识 / 密钥材料。
- **E3 的"格式符合约定"以 §4.3 逆向出的命令集为准**；命令集不明前 E2 / E3 **不可判定**。

## 7. 风险与未核实项

以下各项**在本文写作时均为未核实 / 未定论**，不得当成已知：

1. **设备身份**：已认定为**指纹仪**（§3.1）。**仍开放**：是否兼具读卡功能；两台是否都可用。
2. **厂商 SDK 的许可证与来源未核实**（§4.2）：预编译 `.so` 属供应链风险，默认"保留所有权利"。
3. **`SG_IO` 命令集未逆向**（§4.3）：opcode / CDB / 返回格式均未知；验证需向硬件发命令（须单独授权）。
4. **`/dev/sg0` 的 SELinux 上下文未取证**（§2.1）：当前为 permissive，**enforcing 下可访问性未知**。
   （任务交接写作 `/data/sg0`，实际设备节点为 `/dev/sg0`。）
5. **机 B 的 `/data` 仅余 624M**（见 `docs/research/rk3288-device-capability-survey-2026-09-17.md` §7.1，87% 已用）：**对模板 / 日志落盘空间是现实约束**。
   任何"按 1.7G 规划落盘"的假设在机 B 上会直接踩坑；`RK3288_CONSTRAINTS.md` 的"/data 空间不足导致
   落盘失败"在机 B 上是现实约束而非假想。
6. **`WINUIM` 是否为两台上都可用**：两台的 `sg0` INQUIRY **相同**（本轮取证）；端到端采集
   （设备打开 → 读 SN → 采原始图 → 转 BMP → 生成模板）**记录在
   `docs/research/rk3288-device-capability-survey-2026-09-17.md` §2.3（2026-09-17 机 B 实测）**；
   **本轮 2026-09-18 冻结集未复采**，故**两台是否端到端可用，未验证。**
7. **app 侧 root 通道可用性未取证**（§4.1）：`adb shell` 默认 root ≠ 应用进程能获得 root。
8. **ROM 固有 root 执行面**（本轮取证）：插 U 盘含 `yx_adb.sh` 即以 root 执行；root 边界不由本
   设计独占定义。
9. **`yx_daemon.sh` 的 `check_boot()` 会在开机动画 ≥120 秒时主动 `reboot` 并删 `/data/camera`**
   （见 `docs/research/rk3288-device-capability-survey-2026-09-17.md` §7.5）：外设接入使开机变慢时更易触发，**属需在设计中意识到的设备行为**（非本设计引入）。
10. **dmesg 不可作唯一证据**：两台 dmesg 环形缓冲均已回绕、99% 被看门狗刷屏占满（见 `docs/research/rk3288-device-capability-survey-2026-09-17.md` §7.5）
    ——"dmesg 里查不到" ≠ "没发生过"。

## 8. 不做什么

本文**不包含**、本设计**不授权**以下任何一项：

- **不实施任何代码**：不新增 helper、不新增 `SG_IO` 客户端、不改 JNI / Java / CMake。
- **不向设备发送任何 SCSI 命令**：包括 INQUIRY、`SG_IO` 私有命令、以及对
  `/sys/class/scsi_device/…/{inquiry,vpd_pg80,vpd_pg83}` 的读取（读取即下发命令）。
- **不动读写卡器**：不刷卡、不假设其为读卡器、不为读卡器设计交互。
- **不改任何配置**：不改 `scaling_governor`、不 `setprop`、不 `adb root` / `remount`、不 push / install。
- **不改其它文档**：`docs/index.md`、`docs/RK3288_CONSTRAINTS.md`、`docs/capability-matrix.md`、
  `AGENTS.md`、`CLAUDE.md` 等由其它代理负责，本文只引用其结论（单一事实源见 §1）。

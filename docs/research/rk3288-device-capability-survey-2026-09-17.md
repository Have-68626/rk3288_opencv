<!-- DOCSYNC_DOC
type=research
status=snapshot
as_of=2026-09-17
-->

# RK3288 工控机实测画像与视频管线能力

本文固化 **`yx_rk3288` 实测机**（Android 7.1.2 / API 25，正是本项目目标机型）的一次性采集结果，含**外围硬件识别**、**视频编解码管线能力上限**与**第三方串流方案的路径诊断**。

**为什么固化**：`RK3288_CONSTRAINTS.md` 的目标设备画像长期标注为"待真机溯源"，其 §0.3 列出的"需在设备上采集"字段本文一次性补齐；同时"同一台机器上，不同软件路径的硬件利用率差 10 倍以上"这一结论若不留档，后续极易被重新推导甚至推导错。

> **证据分层（本文严格区分三类，阅读时勿混用）**
>
> 1. **实测值** —— 本机命令采集，附命令，可复现
> 2. **用户观察** —— 使用者主观与客户端上报的数值，非仪器测量
> 3. **未核实** —— 明确标注，不得当结论使用
>
> 所有数字仅对**本实测机**成立；同型号不同批次（ROM、外设装配）可能不同。

---

## 1. 设备与系统

> **机号标注**：本节除随行注明外均为**机 A** 实测（构建指纹 `guo12011713`）；机 B 的差异值随行标注。

| 项 | 实测值 | 来源 |
|---|---|---|
| 型号 | `yx_rk3288`（product / model / device 均为 `rk3288`） | `getprop` |
| 板级 | `ro.board.platform=rk3288`、`ro.hardware=rk30board` | `getprop` |
| 系统 | Android **7.1.2**（API **25**） | `getprop ro.build.version.*` |
| 构建指纹 | `Android/rk3288/rk3288:7.1.2/SUNSHINE/guo12011713:userdebug/release` | `getprop ro.build.fingerprint` |
| 构建类型 | **`userdebug`**、`ro.build.tags=release`，构建时间 2020-12-01 | `getprop` |
| ABI | `armeabi-v7a` | `getprop ro.product.cpu.abi` |
| 内存 | 机 A 2045240 kB、机 B 2045216 kB（≈1.95 GiB） | `/proc/meminfo` |
| 存储 | 机 A `/data` 4.5G（余 **1.7G**，61%）；机 B `/data` 4.5G（**余 624M，87%**）；`/system` 1.9G；`/cache` 122M | `df -h` |
| 面板 | **1920×1080 @ 60.000004 fps（唯一支持模式）**，density 160，带触摸 | `dumpsys display` |
| 显示接口 | `ro.rk.hdmi_enable=true`；`lcdlightservice` 运行中 | `getprop` |

**分区布局**（`/proc/cmdline` 的 `mtdparts`，扇区 512B）：

```text
uboot 4M · trust 4M · misc 4M · resource 16M · kernel 24M · boot 32M
recovery 32M · backup 112M · cache 128M · system 2G · metadata 16M
vendor0 112M · vendor1 112M · userdata ≈4.5G
```

`androidboot.oem_unlocked=0`、`SecureBootCheckOk=0`、`androidboot.selinux=permissive`——**安全启动未强制、SELinux 为宽容模式**。

### 1.1 权限与已装工具（影响可做什么）

- **adb shell 直接是 `uid=0(root)`，SELinux 上下文 `u:r:su:s0`**；`adb root` 报 "already running as root"
- **Magisk 已安装**（`com.topjohnwu.magisk`）、**TWRP 已安装**（`me.twrp.twrpapp`）
- 本项目**已装在设备上**（`com.example.rk3288_opencv`）
- 可用于测量的自带工具：`screenrecord`、`dumpsys`、`top`、`toybox`；**无** `stagefright`、**无** `awk`、**无** `unzip`

### 1.2 设备节点清单（机 A 实测）

| 节点 | 权限 | 说明 |
|---|---|---|
| `/dev/vpu-service` | `crw-rw-rw-` | **Rockchip VPU 视频硬解**，**无需 root 即可访问** |
| `/dev/hevc-service` | `crw-rw-rw-` | H.265 硬解 |
| `/dev/iep` | `crw-rw-rw-` | 图像增强处理 |
| `/dev/rga` | `crw-rw-rw-` | 2D 图形加速 |
| `/dev/mali0` | — | Mali-T764 GPU |
| `/dev/cec0`、`/dev/hdmi_hdcp1x` | — | HDMI CEC / HDCP |
| `/dev/sg0` | **`crw------- root root`** | WINUIM 指纹模块的 SCSI 通道。**普通应用不可访问，必须经 root helper**；本机 SELinux 为 permissive，故仅受 Unix 权限限制 |
| `/dev/video0`、`/dev/video1` | `crw-rw---- media camera` | V4L2 采集节点（属组 `camera`，即**摄像头采集**，非编解码 M2M） |
| `/dev/hidraw0..3` | `crw------- root root` | 机 A：hidraw0/1 = ILITEK 触摸；机 B 另有 c0f4 的两个 |
| `/dev/opteearmtz00` | — | **OP-TEE（ARM TrustZone）**——本机存在可信执行环境 |
| `/dev/gzsd-input` | — | 输入设备（`Phys=gzsd-input/input0`），**用途未明** |
| `/dev/at24`、`/dev/i2c-0..4,6` | — | EEPROM 与 I2C 总线 |
| `/dev/ttyS0..ttyS4` | ttyS0 属 `bluetooth` 组 | SoC UART；**无** `ttyUSB*` / `ttyACM*` |

**不存在的关键节点**：`/dev/mpp*` 与 `/proc/mpp_service`。本机 VPU 走**旧的 `vpu_service` 驱动**（暴露为 `vpu-service` / `hevc-service`），**不是新版 MPP 框架**——这一点直接影响与本项目 `RK_MPP_HOME` 的对接方式。

### 1.3 已装应用（影响"能用什么"）

| 机 | 值得注意的应用 |
|---|---|
| 机 A | 本项目 `com.example.rk3288_opencv`、**Magisk**、**TWRP**、Shizuku、`com.bjw.ComAssistant`（串口助手）、spacedesk、MX Player Pro、Termux |
| 机 B | **`winuim.fingerprint.sample`**（指纹示例，见 §2.3）、**`com.genee.hw_test`**（Giot 硬件测试）、`com.yx.factorytest`、`com.ys.ys_receiver`、`com.adtv`、`com.bjw.ComAssistant`、Termux |

**两台均无**门禁/读卡类应用——这是读卡器"硬件在、软件为零"的直接证据（§2.2）。



---

## 2. 外围硬件识别

> **背景**：本机曾被描述为"有摄像头、麦克风、指纹模块、NFC（疑似一块可刷卡区域）"。**首版本文档的结论（"指纹不存在、刷卡区是 USB 读卡器"）已被实测推翻**，修正版见下表。

> **⚠️ 关于机型与采集出处**：本文数据来自**两台同型号、同类设备**——`192.168.5.2`（构建 `guo12011713`，`device_id_hash=69d83c2ce659`，下文记 **机 A**）与 `192.168.5.3`（构建 `guo08051520`，`device_id_hash=f09acaca146d`，下文记 **机 B**）。**二者属同一类设备**，但 ROM 构建与外设装配存在差异：
>
> | | 机 A (.2) | 机 B (.3) |
> |---|---|---|
> | ROM 构建 | `guo12011713` | `guo08051520` |
> | 外设清单 | 摄像头 + ILITEK 触摸(USB) + USBKey 模块 | 摄像头 + **多一个 `c0f4:06f5` "Usb KeyBoard"** + USBKey 模块 + ILITEK 触摸 |
> | 厂商测试工具 | 无 | **有**（`com.genee.hw_test`、`com.yx.factorytest`、`winuim.fingerprint.sample`） |
> | Magisk / TWRP | 已装 | 未装 |
>
> **同类设备，结论可相互参考**；但涉及具体外设是否装配时，仍应注明采集机号。**§1 除随行注明外为机 A**；**§2 与 §5 的结论来自机 B**（外设齐全、含厂商测试工具的那台）；**§3、§4 的编解码与串流测量来自机 A**。

### 2.1 结论表（机 B）

| 外设 | 实测结论 | 证据 |
|---|---|---|
| **指纹** | ✅ **已确认存在，且在机 B 上经厂商示例应用端到端验证可用**（型号 `WINUIM WMR06` / `WMM06`）；**两台是否都可用未验证** | 见 §2.3 |
| **刷卡区** | ⚠️ **读卡硬件存在且能独立读卡（刷卡有蜂鸣），但主机侧无任何软件消费该数据** | 见 §2.2 |
| **NFC** | ⚠️ **无 Android NFC 协议栈**；但刷卡硬件对 13.56MHz（手机 NFC）有响应，属 NFC 家族技术 | 见 §2.2 |
| **摄像头** | **USB UVC 前置单摄**（`05a3:9230`，Sonix "bftv smart camera"），`uvcvideo` 驱动 | `dumpsys media.camera`、`/sys/kernel/debug/usb/devices` |
| **触摸屏** | ILITEK-TP（USB HID，`hid-multitouch`） | `/proc/bus/input/devices`、`/sys/class/hidraw/*/device/uevent` |
| **额外 HID 设备** | **`c0f4:06f5` "Usb KeyBoard"**——报告描述符为**完整 104 键标准键盘**（KEY 覆盖 0001–00f0 + MSC + LED），非读卡器伪装 | `getevent -d -i /dev/input/event2`（仅机 B 有） |
| 麦克风 | 有（`android.hardware.microphone`；音频编解码 `es8323` / `rt5640`） | `pm list features`、I2C 设备表 |
| 传感器 | 加速度计、陀螺仪（`gs_lis3dh`） | `pm list features`、`/sys/bus/i2c/devices` |
| 以太网 | `eth0` up，MAC `28:52:f9:17:fc:f0`（机 A） | `/sys/class/net` |
| WiFi | `wlan0`（**SDIO**，vendor `0x02d0`），MAC `70:4a:0e:0b:6e:e2`（机 A） | `/sys/bus/sdio/devices` |
| 蓝牙 | **硬件与协议栈存在、当前未启用**（用户通常关闭）：`/sys/class/bluetooth/` 空、rfkill `bt_default` `soft=1`、无 HCI 控制器注册；板级声明见 `rfkill0` 的 DT `bluetooth-platdata`、`DRIVER=rfkill_bt`、`ro.rk.bt_enable=true`。`/dev/rtk_btusb` 是 ROM 携带的 Realtek **软件栈**（`devices/virtual/rtk_btusb`、零接口绑定、`insmod` 在 rc 中被注释），**不是硬件、不构成芯片归属证据** | `/sys/class/bluetooth/`、`/sys/bus/sdio` |
| HDMI | `ro.rk.hdmi_enable=true`、`/dev/cec0`、`/dev/hdmi_hdcp1x`；**未发现 HDMI 输入桥接芯片**（`tc358*` 无命中）→ **输出only** | `getprop`、`/dev` |
| GPU | 芯片实际型号 **ARM Mali-T764 MP4**；驱动自报名字面量 `Mali-T760`；`sys.gmali.version=r18p0-01rel0-x-4@0`；GLES 实测 **`OpenGL ES 3.2`**（`v1.r18p0-01rel0.…`） | `dumpsys SurfaceFlinger` 的 GLES 行、`getprop sys.gmali.version` |

**I2C 总线挂载**（找芯片的最快途径）：`syr827`/`syr828`/`act8846`（电源）、`hym8563`（RTC）、`at24`（EEPROM）、`gs_lis3dh`（加速度计）、`es8323`/`rt5640`（音频）、`ilitek_ts` 与 `Goodix-TS`（**均为触摸控制器，非指纹**）。**无任何读卡器芯片。**

### 2.1.1 本机是「数字标牌」设备，而非通用工控机

厂商系统应用监听的广播动作暴露了 ROM 的性质：

```text
com.ys.ys_receiver : com.ys.setPowerOnTime / dormant_interval / set_static_ip
                     / silent_install / reboot / shutdown / hide|showbar
                     android.56iq.intent.action.*        ← 56iq（数字标牌厂商）
com.adtv           : watchdog / polling / setpoweronoff
                     com.signway.PowerOnOff             ← Signway（标牌硬件厂商）
com.yx.factorytest : /system/app/FactoryTest（v7.1.2）
```

即：**成套的数字标牌 ROM**（定开关机、休眠间隔、看门狗、轮询、远程管理）。另装的 `com.genee.hw_test`（标题「Giot硬件测试程序」）带**门禁功能测试、继电器测试（2 路）、RS485 测试、Zigbee 测试**菜单，但其脚本目录 **`/data/usr/` 不存在**，故测试项无法执行。

### 2.2 刷卡区：硬件可用，但主机侧无软件消费（**修正版**）

**先纠正首版的判断错误**：首版据 SCSI INQUIRY 的 `WINUIM WMR06` + "USBKey Module" + 大容量存储类，**把 `/dev/sg0` 判为读卡器。这是错的**——`WMR06` 是指纹模块（依据见 §2.3）。

**已验证的事实**：

| # | 事实 | 来源 |
|---|---|---|
| 1 | 刷卡**有蜂鸣**，读卡器能独立读卡并本地反馈 | 用户观察 |
| 1a | **刷卡区在机身底部最左侧；指纹模块在底部最右侧**——两者是**两处独立的物理位置**，**不是同一设备** | 用户观察 |
| 1b | 卡片为**感应卡（非接触式）**，无接触/插入区域 | 用户观察 |
| 2 | **用手机 NFC（13.56MHz）刷也能触发蜂鸣** → 读头支持 13.56MHz | 用户观察 |
| 3 | 刷卡在 **USB 中断**上零活动：`ehci_hcd:usb2` 在 120 秒窗口内恒定 1372 | 实测 |
| 4 | 刷卡在 **HID 事件节点**（event2/3）零字节 | 实测 |
| 5 | 刷卡在 **HID 原始报告**（hidraw0/1）零字节 | 实测 |
| 6 | 刷卡在 **四个 SoC UART**（ttyS1–ttyS4）零字节 | 实测 |
| 7 | **无 Android NFC 协议栈**：无 `android.hardware.nfc` feature、无 NFC 服务、无 NFC HAL | 实测 |
| 8 | **无任何读卡软件**：`/system/lib` 仅有 AOSP 自带的 `libnfc_ndef.so`；已装应用无门禁/读卡类 | 实测 |
| 9 | 串口 `ttyS3` 曾有计数增长（tx 8→32、rx 36→144），但**刷卡期间实测不变** → **不能归因于刷卡** | 实测 |

> **方法学要点（必须记住）**：第 4~6 条的阴性结果**有阳性对照**——同期用**真实手指触摸**触摸屏，在 `event6` 上抓到 9600 字节真实 evdev 事件。因此"抓不到"不是工具失灵。
>
> 另有一条反面教训：`input tap` 注入的事件**不经过 evdev**，故不能用它做 evdev 采集的自测。

**结论**：读卡器是**能独立工作的自带模块**（读卡 + 本地蜂鸣不需主机参与），但**当前 ROM 里没有任何软件消费卡数据**，因此刷卡对 Android 主机完全不可见。这不是硬件故障，也不是接口未找到。

**可能的连接路径（均未证实）**：① 与指纹共用 `22bc:2009` 模块、走其 SCSI 通道——**2026-09-18 起不倾向此路径**：`/dev/sg0`（`22bc:2009`）已认定为**指纹仪**（§2.3、§7.8），而读卡区在机身底部**最左侧**、指纹在**最右侧**，两者是**独立的物理位置**（见上表 1a）；② **Wiegand 接口（GPIO 数据线）**——门禁读头最常见做法，**不产生任何串口/USB 流量**，与本观察完全吻合；③ 挂在某 UART 上但由缺失的门禁服务驱动。

**启用它的可行路径**：取得厂商门禁应用/固件，或逆向读卡器协议（见 §2.4）。

### 2.3 指纹：已确认是指纹仪，且机 B 上端到端可用（**修正版**）

**首版判"未发现任何软件或硬件证据"，已推翻。** 关键线索是**厂商自带指纹 SDK 一直在设备上**，且其界面首启为黑屏导致未能识别。

| 项 | 实测 |
|---|---|
| 应用 | `winuim.fingerprint.sample` v1.3.0（`minSdk=11`、`targetSdk=18`，2013 年代的应用） |
| 应用标题 | **`Winuim WMR06/WMM06 Fingerprint Sample`** |
| 权限 | `android.permission.USBFS`、读写外部存储 |
| 设备 | `/dev/sg0`，SCSI INQUIRY：**`WINUIM` / `WMR06`**，type 5，rev 0110 |
| USB | `22bc:2009`，"USBKey Chip / USBKey Module"，SerialNumber `2027300413` |
| 支持模式 | **usb 模式** 与 **串口模式**（界面可选，实测用 usb 模式成功） |

**SDK 结构**（`/data/app/winuim.fingerprint.sample-1/lib/arm/`）：

```text
libJNIFp_Sdk_WMR08.so    JNI 胶水层，映射 winuim.fingerprint.sdk.FpScannerWMR08.*
libftrScanAPI.so         Futronic 扫描仪 API（错误串含 CFs80CompatibleDevice、CFsUsb20t80HDevice）
libfp_sdk.so             主 SDK（Delphi 编译，含 SU_QueryUSBVID / SU_QueryUSBPID）
libscsiusb.so            SCSI-over-USB 封装（SU_OpenUSBHandle / SU_SendUSBCMD）
libusb-1.0.so            直接 USB 访问
libserial_port.so        串口支持
```

> **`libscsiusb.so` 是识破装置性质的关键**：**Futronic 系指纹扫描仪本就以 SCSI 设备形态出现**——这正是该库名的由来。首版漏掉这条线索，才把 `/dev/sg0` 误判为读卡器。

**端到端验证（机 B 实测）**：

| 步骤 | 结果 |
|---|---|
| 打开设备 | 成功 |
| 读取 SN | `13295244-1957-A735-3A3B-1EB769FFB6E4` |
| 采集指纹 | 日志 `FPDevRawToBmp2 begin/1..4/end` —— 原始图转 BMP |
| 生成模板 | 日志 `已经进行到了GenerateTemplate方法` |
| 界面状态 | **提取指纹特征成功** |
| 图像 | **真实指纹图像已渲染**（脊线可见），标注「采集成功」 |
| 模板内容 | 十六进制串，**头部 `57494E55494D` = ASCII "WINUIM"** |

**结论**：**该模块已确认是指纹仪**（读卡器不可能产出带脊线的指纹图像），链路为 设备打开 → 读 SN → 采原始图 → 转 BMP → 生成模板。**但**：`/dev/sg0` 权限为 `crw------- root root`，接入仍**须经 root 通道**；且**两台是否都可用未验证**（端到端仅机 B 测过）。

### 2.4 读卡器逆向计划（非破坏性，待执行）

> **硬约束**：无原厂技术支持，且**不得对设备造成任何破坏**。因此本计划**只做只读探测**。任何**向硬件写入**的操作（向串口/USB 发送命令、加载内核模块、刷写分区）**必须单独确认后执行**。
>
> **风险控制**：不动分区、不写 flash、不改系统文件；探测仅读 `/proc`、`/sys`、`/dev` 的只读属性。

**目标**：确定读卡器的宿主接口，进而读出卡号。

**第一步（只读）——判定接口**

| 探测 | 若命中说明 |
|---|---|
| 全量 `/proc/interrupts` 差分（刷卡窗口内） | 某条未命名中断随刷卡增长 → 读卡器由**中断/GPIO 驱动**，即 **Wiegand**（D0/D1 双线）的典型特征 |
| `/sys/class/gpio/` 导出项与方向 | 是否存在被占用的 GPIO 线 |
| `/proc/device-tree/` 中 `wiegand` / `card` / `rfid` 节点 | 设备树是否声明了读卡器 |
| `/proc/tty/driver/serial` 累计 `tx`/`rx` | **已做**：刷卡期间无变化 → **排除 ttyS1–ttyS4** |

**第二步（只读）——找消费方**
扫描 `/proc/<pid>/fd/*` 是否持有串口/USB 节点；检查 `com.ys.ys_receiver`、`com.yx.factorytest`、`com.adtv` 等厂商组件的字符串中是否有卡号/读卡关键字。

**第三步（仅在前两步定位到接口后才做）——协议解析**
若定位到串口，需先确认**波特率与帧格式**（门禁读头常见 9600 / 19200 / 115200，8N1）。**向串口写入字节属于对硬件的写操作，执行前单独确认。**

**备选方向**：`hw_test` 的「RS485测试」项与 `ComAssistant`（串口助手）的存在，提示厂商工程手段走 RS485；若能取得门禁应用的 APK，其字符串可直接给出协议。

**另一条低风险路径**：该读卡器支持 13.56MHz（手机 NFC 可触发）。若后续确认它挂在某 UART 上，**最省事的验证是用一部手机运行 NFC 读卡 App 直接读卡号**——完全绕开本机逆向。

---

## 3. 视频编解码管线能力

### 3.1 硬解通路存在且可用

`media.codec` 服务（PID 237）在播放期间加载：

```text
/system/lib/librk_vpuapi.so          ← Rockchip VPU API
/system/lib/libvpu.so                ← VPU 驱动封装
/vendor/lib/libomxvpu_dec.so         ← VPU 硬件解码器 OMX 组件
/vendor/lib/libOMX_Core.so + libRkOMX_Resourcemanager.so
/vendor/lib/libgralloc_priv_omx.so + /system/lib/libdrm_rockchip.so  ← 零拷贝到显示
```

**未加载任何 `libstagefright_soft_*` / `OMX.google.*` 软件解码器。**

设备节点：`/dev/hevc-service`(244,0)、`/dev/vpu-service`(245,0)，**权限均为 `crw-rw-rw-`（无需 root）**（2026-09-18 复核更正 major/minor，见 §7.7）。另有 `/dev/iep`（图像增强）、`/dev/rga`（2D 加速）、`/dev/mali0`。

### 3.2 声明上限（换算后的可用工作点）

`/system/etc/media_codecs.xml`（注释边界在 2–16 与 18–77 行，**87 行之后为真实声明**）：

| 项 | 声明 |
|---|---|
| H.264 硬解（`OMX.rk.video_decoder.avc`） | size ≤ 4096×2160；`blocks-per-second` ≤ **244800**；`block-size` 16×16；bitrate ≤ **10 Mbps** |
| H.265 硬解（`OMX.rk.video_decoder.hevc`） | 同上 |
| H.264 硬编（`OMX.rk.video_encoder.avc`） | ≤ 1920×1088；bitrate ≤ 40 Mbps；并发 4 |

**换算**：244800 block/s × 256 px = 62.67 Mpixel/s →

- **1080p**（2,073,600 px/帧）→ **30.2 fps**
- **720p**（921,600 px/帧）→ 68 fps（受面板 60 封顶）

### 3.3 实测上限（标定方法 + 结果）

**标定方法**：用 `screenrecord` 生成**帧数已知**的素材，再用 `/proc/interrupts` 的 `vpu-service` 中断增量测播放时的解码速率。由于素材帧率已知，可反推"中断 : 帧"的比值。

| 素材 | 已知帧率 | 播放时 VPU 中断 | 结论 |
|---|---|---|---|
| 720p（1577 帧 / 30s） | 52.6 fps | **54.9 /s** | **比值 ≈ 1 中断/帧（标定成立）** |
| 1080p（用户自有视频） | — | **31.5 /s** | → **≈31 fps** |

| 结论 | 值 |
|---|---|
| **1080p 硬件解码** | **≈ 31 fps** |
| **720p 硬件解码** | **≈ 55 fps** |
| 解码的 CPU 代价 | `media.codec` **≈1%** —— **硬解几乎不占 CPU** |
| 1080p 硬件编码（旁证） | 实测 `screenrecord` 29 fps |
| 720p 硬件编码（旁证） | 实测 `screenrecord` 53 fps |

**三条独立信号收敛**：声明上限算出 30fps、实测解码 31fps、实测编码 29fps。**先前"声明的 30fps 可能只是照抄模板、实际更宽松"的猜测被推翻——硬件确实卡在 1080p30。**

**由此得到的档位**：`1080p60` **不可行**（差约 2 倍）；`1080p30` 可用且与面板 1:1；`720p60` 可用。

### 3.4 一个易踩的坑：`screenrecord` 看不到走 HWC 的视频层

播放视频时 `screenrecord` 只录到 **1 帧 / 10 秒**，而 `screencap` 同一时刻的两张截图哈希不同（画面确实在动）。原因是视频层经硬件叠加（HWC）直通显示，**被 screenrecord 的虚拟显示排除**。因此**不能用录制帧数反推播放帧率**，必须走 §3.3 的中断法。

---

## 4. spacedesk 路径诊断（本机当前在用的方案）

### 4.1 用户观察（非仪器测量）

| 项 | 观察值 |
|---|---|
| 分辨率 / 画质 | 1920×1080，质量 70% |
| 帧率 | 最高可达 60fps；**随内容变化面积动态波动** |
| 小范围内容变化 | 可维持较高帧率 |
| **大范围移动**（如拖动窗口横穿） | **掉至 20–45fps，画面模糊、卡顿** |
| 输入模拟 | 支持，但**有可感知延迟** |

**这一现象本身就是诊断**：帧率取决于"屏幕改动了多少"而非固定的解码吞吐上限 → **瓶颈在每帧数据量，不在解码器算力**。

### 4.2 实测（活跃串流会话中，12 秒中断增量）

| 硬件块 | 速率 | 含义 |
|---|---|---|
| `arch_timer` | 3153/s | 正常 |
| **`eth0`** | **1288/s** | **网络接收中断密度极高** |
| `ff650000.i2c` | 272/s | 触摸控制器轮询 |
| **`ffa30000.gpu`（Mali）** | **~400/s**（两条线合计） | **GPU 被大量占用** |
| `ff940000.vop`（显示控制器） | 61.7/s | 面板 60Hz 刷新 |
| **`vpu-service`（视频硬解）** | **0** | **完全未使用** |

同时：spacedesk 进程 **51–55% 单核**；**整机 Idle 仅剩 13%**（`User 486 + Sys 200 + Idle 108`）；`surfaceflinger` 6–7%。

进程加载库：`libjpeghwdec.so`、`librk_vpuapi.so`、`libmediandk.so`、`libstagefright*`——**无 ffmpeg/avcodec**（故非软件视频解码），但也**未驱动任何硬件解码块**。

### 4.3 结论

**spacedesk 走的是 CPU + Mali GPU 路径，不使用 Rockchip 的视频硬件解码。** 结合用户观察，瓶颈是**每帧数据量**（帧内编码导致大范围变化时单帧极大），经 CPU 上传与 GPU 合成、并伴随高密度网络中断。

**对比基准（同机同内容）**：

| 路径 | CPU | 硬件利用率 |
|---|---|---|
| VPU 硬解（实测） | `media.codec` **≈1%** | VPU 承担 |
| spacedesk（实测） | 进程 51–55%，**整机 Idle 13%** | **VPU 为 0**，GPU ~400/s |

**即：同一台机器，走 VPU 时 CPU 几乎空闲；当前方案把整机用到近乎满载，却没用到视频硬件。** 硬件能力（1080p31 / 720p55）本身是够的。

**对后续选型的指向**（本文不预设结论）：时域编码（H.264/H.265）的价值在于**压缩效率比帧内编码高 1–2 个数量级**，可使"大范围变化"这一最坏情况的开销回落到与"小范围变化"同量级——这正是当前形态下的病根。

---

## 5. 复现命令

### 5.1 设备画像与外设

```bash
# 身份（两台构建不同，务必同时记录 fingerprint 与 serialno 以区分机号）
adb shell "getprop ro.build.fingerprint; getprop ro.serialno; getprop ro.board.platform"
adb shell "cat /proc/cmdline"                     # 含分区布局 mtdparts 与安全启动状态
adb shell "cat /proc/meminfo | head -4; df -h /data /system"

# 面板模式与刷新率（本机仅一个模式）
adb shell "dumpsys display | grep -E 'supportedModes|fps=|PhysicalDisplayInfo' | head"

# 设备节点与权限（注意 sg0 / hidraw 的属主）
adb shell "ls -l /dev/ | grep -iE 'vpu|hevc|iep|rga|mali|sg[0-9]|hidraw|video|ttyS|optee|mpp'"

# 输入设备与 HID 报告描述符（-d 显示 descriptor）
adb shell "cat /proc/bus/input/devices"
adb shell "getevent -d -i /dev/input/event2"

# 外设识别
adb shell "for d in /sys/bus/i2c/devices/*/; do echo \"\$(basename \$d)=\$(cat \$d/name)\"; done"
adb shell "cat /sys/kernel/debug/usb/devices | grep -E 'T:|P:|S:|Cls='"
adb shell "for h in /sys/class/hidraw/hidraw*; do cat \$h/device/uevent | grep -E 'HID_NAME|HID_ID'; done"
```

### 5.2 硬解能力与解码速率

```bash
# 硬解通路是否在（应见 librk_vpuapi / libvpu / libomxvpu_dec，且无 soft 解码器）
adb shell "grep -oE '/[^ ]*\.so' /proc/\$(pidof media.codec)/maps | sort -u | grep -iE 'vpu|omx|stagefright'"

# 声明上限（注释边界之后的才是真实声明）
adb shell "grep -nE -A8 'video/avc' /system/etc/media_codecs.xml | head -20"

# 解码速率（核心测量：VPU 中断差分。1 中断 ≈ 1 帧，须用已知帧率素材标定）
adb shell "cat /proc/interrupts" > i1.txt; sleep 10; adb shell "cat /proc/interrupts" > i2.txt
# 对比 vpu-service 行增量 / 10 ≈ fps

# 哪条路径在用硬件（全中断差分，按增速排序）
adb shell "cat /proc/interrupts" > a.txt; sleep 12; adb shell "cat /proc/interrupts" > b.txt
```

### 5.3 采集事件与字节（**方法关键，易错**）

```bash
# 生成已知帧数的测试素材（screenrecord 走硬件编码；帧数由内容变化量决定，需持续运动）
# 让滑动循环存活需保持 adb shell 不退出（后台化 + sleep）——`adb shell "cmd &"` 会在连接断开时被杀
adb shell "screenrecord --size 1920x1080 --bit-rate 8000000 --time-limit 40 --verbose /sdcard/t.mp4"
# --verbose 会打印 "recorded N frames"，据此算素材帧率

# 无缓冲采集（evdev / hidraw / 串口通用）：dd 直读设备节点，边读边落盘，被 kill 也保留
adb shell 'timeout 120 dd if=/dev/input/event6 of=/data/local/tmp/ev6.bin bs=16 count=100000 2>/dev/null &
           timeout 120 dd if=/dev/hidraw0 of=/data/local/tmp/hd0.bin bs=64 count=2000 2>/dev/null &
           sleep 125'
adb shell "ls -l /data/local/tmp/*.bin; od -An -tx1 -v /data/local/tmp/ev6.bin | head"

# 串口累计计数（不依赖采样时机；rx>0 即该口曾有数据进来）
adb shell "cat /proc/tty/driver/serial"
```

> **三个必须避开的坑（均已实际踩过）**
>
> 1. **`getevent` 输出到管道是块缓冲**——用 `kill` 终止时缓冲区被丢弃，结果恒为 0 字节。**改用 `dd` 直读设备节点**（无缓冲），或让 `getevent -c N` 自然退出。
> 2. **`input tap` 注入的事件不经过 evdev**——故**不能**用它验证 evdev 采集是否可用。**必须用真实手指触摸做阳性对照**（实测触摸在 `event6` 上产生 9600 字节）。
> 3. **累计计数器必须与目标动作在同一时间窗口内对齐**——否则不构成因果（`ttyS3` 的教训，见 §6）。

### 5.4 指纹模块验证（机 B）

```bash
adb shell "pm path winuim.fingerprint.sample"
adb shell "ls -l /data/app/winuim.fingerprint.sample-1/lib/arm/"     # SDK 库清单
adb shell "for f in vendor model type rev; do cat /sys/class/scsi_generic/sg0/device/\$f; done"
# 旧应用首启可能黑屏；force-stop 后干净重启可恢复界面
adb shell "am force-stop winuim.fingerprint.sample"
adb shell "am start -n winuim.fingerprint.sample/.MainActivity"
adb shell "logcat -d | grep -E 'FPDev|jniFP|GenerateTemplate'"       # 采集时的关键日志
```

**两条环境注意**：① Linux/Windows 下 adb 与 Python 对 `/tmp` 的解释可能不一致（MSYS 的 `/tmp` ≠ Windows Python 的 `/tmp`），脚本中请用绝对路径；② Git Bash 会把 `/sdcard/...` 误当本地路径转换，需 `MSYS_NO_PATHCONV=1`。

---

## 6. 未能核实 / 待查项

1. **读卡器的宿主接口未确定**（最高优先待查）。已**排除**：USB 中断活动、USB HID 事件节点（event2/3）、HID 原始报告（hidraw0/1）、全部 SoC UART（ttyS1–ttyS4）、I2C 芯片、SPI 设备、设备树节点（无 `wiegand`/`card`/`rfid`）、GPIO 导出线（仅有 gpiochip，无导出项）。**待查**：是否与指纹共用 `22bc:2009` 模块的 SCSI 通道；是否走 Wiegand（GPIO 双线）但由**独立 MCU** 解码（故本机内核无感知）；是否存在未枚举的内部 USB 口。逆向计划见 §2.4。
2. **`c0f4:06f5` "Usb KeyBoard" 的用途未确定**：其报告描述符为完整 104 键标准键盘（非"只发数字"的读卡器伪装），且**机 A 上不存在该设备**。它是否为读卡器的 HID 化身？——**刷卡期间它在 event2/3 与 hidraw0/1 上均为零字节，故该假设不成立**；其真实用途仍未知。
3. **`WMR06`（指纹）是否兼具读卡功能未验证**：理论上可通过 `/dev/sg0` + SG_IO 发送私有 SCSI 命令探测，但那属于**向硬件写入**，按 §2.4 约束须单独确认后执行。
4. **设备曾在采集期间重启一次（2026-09-18 已确认关闭）**：uptime 归零，但 `pstore`/ramoops **无任何崩溃记录**。**已由用户澄清**：那是**到熄灯时间用户手动操作设备断电**造成的，**不是设备故障**（断电不会留下崩溃记录，与"uptime 归零 + pstore 无记录"完全一致）。→ 原文"若计划长时间连续运行，这是需要留意的稳定性信号"的担忧**予以撤销**——它不是稳定性问题。另：dmesg 里刷屏的 `time_cnt==0 ,wdt_timer==-1` 并**不是**看门狗故障线索（详见 §7）。
5. **蓝牙（2026-09-18 已定论）**：**设备有蓝牙硬件与协议栈、当前未启用**（用户通常关闭）——`/sys/class/bluetooth/` 空、rfkill `bt_default` `soft=1`、无 HCI 控制器注册；板级声明见 `rfkill0` 的 DT `bluetooth-platdata`、`DRIVER=rfkill_bt`、`ro.rk.bt_enable=true`。原"两者矛盾"的表述**作废**：`/dev/rtk_btusb` 是 ROM 携带的 Realtek **软件栈**（`devices/virtual/`、零接口绑定、`insmod` 在 rc 中被注释、驱动编进内核），**不是硬件、不构成芯片归属证据**；芯片确切型号**仍未定论**（SDIO 侧枚举 Broadcom `02D0:A9A6`，与 ROM 的 Realtek 栈**层级不同、不能互相推断**），启用态可用性**未取证**。
6. **WiFi/BT 在主线 Linux 下的驱动可用性**——未测。
7. **`libjpeghwdec.so` 是否实际参与 spacedesk 的解码**：库已加载，但无对应硬件中断增长；其"hw"之名与实测行为未对齐，未查清。
8. **spacedesk 的实际传输编码**（是否确为帧内 JPEG）：由"无任何硬件解码块 + 帧率随内容变化面积波动"强烈提示，但**未抓包验证**。
9. **HDMI 输入**：仅依据"未发现 `tc358*` 桥接芯片"推断为只输出，未做物理验证。

> **一条方法学更正（已并入正文）**：`ttyS3` 曾出现 `tx 8→32 / rx 36→144` 的计数增长，一度被误当作读卡器线索；后续在**刷卡窗口内实测其完全不变**，故该增长**不能归因于刷卡**（很可能是打开串口本身引起的驱动侧计数，或浮空 RX 引脚的噪声）。**教训：累计计数器的变化必须与目标动作在同一时间窗口内对齐，否则不构成因果。**

---

## 7. 2026-09-18 复核与补遗

2026-09-18 对两台同类实机（`192.168.5.2` / `192.168.5.3`）做了一次**只读取证**复核（主机时钟窗 11:54–12:15 +0800；对设备的写操作为零，未 `adb root` / `remount` / `setprop`，未 push/install）。逐条修正与补全如下（均为"原值 → 修正值"）：

> **`as_of` 说明**：本快照 `as_of=2026-09-17` **保持不变**——那是**采集日**。本节及正文中由 2026-09-18 复核引入的更正（例如 §1 新增的机 B `2045216 kB` / `624M`）**以本节为准**；**头部日期 09-17 不覆盖 09-18 的复核口径**。

### 7.1 序列号去标识 与 机号归属补全

- **明文序列号**：原文 §2 前言记有**明文**设备序列号 → 改为去标识 `device_id_hash`（SHA-256 前 12 位小写 hex，口径同 `scripts/run-android-face-regression.ps1` 的 `Get-Sha256Short`）：**机 A `69d83c2ce659`**、**机 B `f09acaca146d`**。此后全文只出现 `device_id_hash`，不再出现明文序列号。
- **§1 整表机号**：原文 §1 落在"仅机 A / 仅机 B"的归属空白里；实测其构建指纹 `guo12011713` 属**机 A**，已在 §1 加机号标注。
- **内存 `MemTotal`**：原只记 `2045240 kB`（机 A）→ 新增机 B `2045216 kB`。
- **`/data` 容量**：原只记 `4.5G 余 1.7G`（机 A）→ 新增机 B `4.5G 已用 3.9G、**余 624M（87%）**`。这正是"按 1.7G 规划落盘"会在机 B 踩坑的原因；`RK3288_CONSTRAINTS.md` 的"`/data` 空间不足导致落盘失败"在机 B 属现实约束。

### 7.2 GPU / GLES 需分两层（原文把两层混为一谈）

- **GLES 版本**：原 `3.0` → 实测两台逐字一致：`GLES: ARM, Mali-T760, OpenGL ES 3.2 v1.r18p0-01rel0.5ff61f1c424963a640f75d9e10d6cb79`，即 **`OpenGL ES 3.2`**。`GLES 3.0` 只是本项目自设的保守渲染基线。
- **GPU 名称**：驱动/SurfaceFlinger **自报名字面量为 `Mali-T760`**，而芯片实际型号是 **`Mali-T764 MP4`** → 二者必须分列，否则引用时互相打架。
- `sys.gmali.version` = `r18p0-01rel0-x-4@0`（两台）。

### 7.3 蓝牙：不是"不存在"，是"存在但未启用"

- 原表述（§2.1 与 §6-5）：`/dev/rtk_btusb`（Realtek）与 SDIO 的 Broadcom "**矛盾，未核实**" → **作废**。
- **用户澄清（2026-09-18）**：**设备本身有蓝牙功能，用户通常直接关闭不用。**
- 板级确实声明了蓝牙支持：`rfkill0` 的 DT compatible 为 `bluetooth-platdata`、`DRIVER=rfkill_bt`、`ro.rk.bt_enable=true`。
- 测量时看到的是**未启用态**：`/sys/class/bluetooth/` 为空、rfkill `bt_default` `soft=1` 软阻塞、无 HCI 控制器注册。→ **正确表述是"蓝牙硬件与协议栈存在、当前未启用"，不得写成"无蓝牙"或"无可用控制器"。**
- `rtk_btusb` 的结论**保留**：其 sysfs 路径为 `devices/virtual/rtk_btusb`（virtual 总线，背后无物理设备）、零接口绑定、`insmod` 行在 rc 中被注释（驱动编进内核）→ 它是 ROM 携带的 Realtek 蓝牙**软件栈**，**不构成芯片归属证据**。
- **芯片确切型号仍未定论**：SDIO 侧枚举到 Broadcom `02D0:A9A6`（`SDIO_CLASS=00`），而 ROM 携带的 BT 栈是 Realtek（`bt_vendor.conf: UartPort=/dev/ttyS0`、`rtkbt.conf: BtDeviceNode=/dev/rtk_btusb`）——**两者层级不同，不能互相推断**。启用态可用性**未取证**。

### 7.4 外设补遗

- **指纹设备两台都有**：`sg0` 的 SCSI INQUIRY 两台均为 `WINUIM` / `WMR06` / `type 5` / `rev 0110`；原文给人的印象是"机 B 独有"。
- **`/dev/sg0` 接口真相**：声明为 USB Mass Storage / Bulk-Only（`08/06/50`），但**内核无 `sd_mod`** → `usb-storage` 绑定后**不产出任何块设备**，只剩 `/dev/sg0` 一个 SCSI generic 节点。
- **读卡器"零流量"的内核级硬证据**：机 B `/sys/class/scsi_device/0:0:0:0/device/` 的 `iorequest_cnt=0x1`（自开机只收到枚举那 1 条 INQUIRY）。
- **`scaling_governor` 两台四核全为 `interactive`** → 基准的受控前提**当前不满足**；本轮未写该值（对设备的写操作未授权）。
- **`WINUIM WMR06` 的身份：已可认定为「指纹仪」**（口径收敛见 §7.8）。依据是 2026-09-17 机 B 用厂商示例应用**端到端采到了指纹**——读 SN、采集原始图、转 BMP、生成模板，且界面**渲染出了带脊线的真实指纹图像**（§2.3）；**读卡器不可能产出指纹图像**。另有厂商 SDK 的 Futronic 系证据：`libftrScanAPI.so`、错误串含 `CFs80CompatibleDevice` / `CFsUsb20t80HDevice`。
- **仍未验证的是**：① 它**是否兼具读卡功能**（§6-3）；② **两台是否都可用**（端到端仅机 B 测过）。另两个指纹相关 APK（`winuim.fingerprint.sample`、`com.genee.hw_test`）装在 `/data/app/`、时间戳 `2021-08-25` 晚于 ROM 构建 `2021-08-05`，说明机器**事后被侧载过**。

### 7.5 §6-4 的重启悬案：已由用户澄清关闭（不是设备故障）

- **结论**：2026-09-17 采集期间的那次重启 = **用户到熄灯时间手动操作设备断电**，**非设备故障**；uptime 归零 + `pstore`/ramoops 无崩溃记录与此完全一致（断电不会留下崩溃记录）。→ 原文"若计划长时间连续运行，这是需要留意的稳定性信号"**予以撤销**。
- **`check_boot()` 的行为仍要记录**（已读源脚本逐行核实、属**已核实的潜在风险**，但**不是**那次重启的原因，勿混为一谈）：开机动画持续 **≥120 秒**时走**正常分支** `rm -r /data/camera; rm /etc/media_profiles.xml; sync; reboot`（**主动 reboot**）；异常分支（`/data` 未挂载/只读、或 `ro.crypto.state=encrypted`）才是 `rm -rf /data/*` + `mke2fs` 重建 userdata。触发条件在"接了多个 OTG 设备"或"`/data` 接近写满导致挂载变慢"时更易满足。
- ⚠️ **易误读点（必须写清）**：脚本的 `unit` 判据取的是 `df -h` **第 2 列（Size 列）**的末字符，**不是剩余空间**；两台 Size 均为 `4.5G` 故 `unit=G`，**不触发清空分支**。**不要**写成"剩余空间小就会清空"。
- **dmesg 刷屏的线索更正**：`time_cnt==0 ,wdt_timer==-1` 来自 `ping_test()` 每 30 次轮询执行的 `echo wdt > /proc/oem_gpio`，是**该 ROM 的普通周期行为，不是故障信号**（原文把它列为"可能是硬件看门狗"的线索）。
- **方法学**：dmesg 环形缓冲已回绕（两台最早一行分别是 `[5999.216154]` 与 `[6180.837065]`）→ **"dmesg 查不到" ≠ "没发生过"**，开机期证据必须交叉验证。

### 7.6 本轮两台均未重启

- 2026-09-18 复核期间**两台均未重启**：机 A uptime `15684.10`，机 B uptime `15707.97` → `16814.09`（主机墙钟 Δ≈1106s 与 uptime Δ≈1106s 完全吻合）；`adb devices` 首尾 transport_id 不变。

### 7.7 设备节点 major/minor 更正（本轮现场查实）

- **原值**（§3.1）：`/dev/vpu-service`(244,0)、`/dev/hevc-service`(245,0)。
- **修正值**：`/dev/hevc-service`(244,0)、`/dev/vpu-service`(245,0)——二者**对调**。
- **依据**：2026-09-18 两台设备只读复核 `ls -l /dev/ | grep -E "vpu-service|hevc-service"`，逐字输出为：

```text
crw-rw-rw- 1 media media 244,   0 hevc-service
crw-rw-rw- 1 media media 245,   0 vpu-service
```

- **权限位与"无需 root"的结论不变**（两个节点均为 `crw-rw-rw-`）。

### 7.8 指纹身份口径收敛（本轮修正原"身份未定论"）

- **原值**（§7.4 首版与本节末的"仍未定论"清单）：`WINUIM WMR06` 是"指纹仪还是读卡器**仍未定论**"，并要求"不得把'是指纹模块'写成单向结论"。
- **修正值**：**身份已可认定为「指纹仪」**。依据是 2026-09-17 机 B 用厂商示例应用**端到端采到了指纹**——读 SN、采集原始图、转 BMP、生成模板，界面**渲染出带脊线的真实指纹图像**（§2.3）；**读卡器不可能产出指纹图像**。厂商 SDK 亦为 Futronic 系（`libftrScanAPI.so`、错误串含 `CFs80CompatibleDevice` / `CFsUsb20t80HDevice`）。
- **未验证项收窄为两项**：① 它**是否兼具读卡功能**（§6-3）；② **两台是否都可用**（端到端仅机 B 测过）。
- **依据出处**：§2.3 的端到端指纹图像证据 + 厂商 SDK 的 Futronic 系证据 + **物理位置独立**（§2.2 表 1a：读卡区在底部最左侧、指纹在底部最右侧，两者不是同一设备）。

> **本轮仍未取证/未定论（不得当结论）**：`WINUIM WMR06` 是否兼具读卡功能、两台是否都可用（**身份已认定为指纹仪**，见 §7.8）；`c0f4:06f5` 是否为读卡器的"键盘楔"；读卡器的宿主接口；蓝牙芯片确切型号与启用态可用性；WiFi/BT 在主线 Linux 下的驱动可用性；`libjpeghwdec.so` 是否参与 spacedesk 解码；HDMI 是否真的只输出；`/data/camera` 与 `/etc/media_profiles.xml` 当前是否存在。

## 8. 与其他文档的关系

- [RK3288_CONSTRAINTS.md](../RK3288_CONSTRAINTS.md) —— 目标设备画像与工程硬约束；本文为其 §0.3「需在设备上采集」字段的一次性补齐，**该文档 §0.x 已随之改为『已固化』口径**（去标识序列号见本文 §7.1）
- [capability-matrix.md](../capability-matrix.md) —— 各终端能力矩阵
- [opensource-integration-feasibility-2026-09-16.md](opensource-integration-feasibility-2026-09-16.md) —— 开源可集成性评估

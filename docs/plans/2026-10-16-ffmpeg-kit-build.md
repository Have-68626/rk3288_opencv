<!-- DOCSYNC_DOC
type=plan
status=living
as_of=2026-10-16
-->

# FFmpegKit Android 本地源码构建

用户明确选择沿用现有 `libx264` 调用，启用 GPL，编译 ARMv7 与 ARM64。本轮完成本地 AAR 及 APK 集成验证；未提交、未推送，未连接设备或执行推流。

## 输入与产物

- 用户提供 `cache/ffmpeg-kit-6.0.LTS.zip`，11,950,068字节，SHA-256：`22fe0791ed5fa9cac8d55b598e3776e2befa1daa2efa3dfbb396e82f0011aade`。未另行将它与远端归档逐字节比对，构建标签明确使用 `6.0.LTS-source-zip-22fe0791`。
- 成品：`app/libs/ffmpeg-kit.aar`；副本：`cache/ffmpeg-kit-build-2026-10-16/ffmpeg-kit.aar`。二者SHA-256一致，为 `07dd0dcffbe52023b614e7455694093e9d17003626247f3ac9361b28aaf57eda`；大小26,386,523字节，约25.16MiB。
- ABI仅 `armeabi-v7a` / `arm64-v8a`，共26个原生库。ARMv7同时保留普通与NEON的FFmpeg库；JNI层按本项目契约要求NEON。最低API21，**不宣称兼容非NEON设备或上游通用LTS的API16范围**。
- 自包含Java8 JAR由42份源码编译，含FFmpegKit、smart-exception-java/common 0.2.1，共45个类；不依赖本地AAR无法传递的Maven POM。许可证随AAR保留。
- 本次启用x264/GPL，FFmpeg构建为GPL 3.0或更新版本；OpenSSL为Apache-2.0，smart-exception为BSD-3-Clause。用户已明确批准该构建选择。

## 实际有效源码

| 组件 | 上游固定引用 | 实际commit |
|---|---|---|
| arthenica/FFmpeg | n6.0 | `ea3d24bbe3c58b171e55fe2151fc7ffaca3ab3d2` |
| arthenica/x264 | 固定commit | `eaa68fad9e5d201d42fde51665f2d137ae96baf0` |
| arthenica/openssl | openssl-3.1.1 | `2cf4e90eaaf7402bf038b158dbdacd0a15561fb7` |
| arthenica/cpu_features | v0.8.0 | `936b9ab5515dead115606559502e3864958f7f6e` |

主机构建环境为既有Ubuntu WSL，新增必要构建工具及OpenJDK17；Java以Android34的 `android.jar` 作为引导类库编译为Java8字节码。原生编译使用Linux NDK r25c（25.2.9519653），归档SHA1与[Android官方历史下载页](https://github.com/android/ndk/wiki/Unsupported-Downloads)的 `53af80a1cce9144025b81c78c8cd556bff42bd0e` 匹配；完整SHA256与依赖来源在缓存 `build-inputs.json`。

WSL原生工作目录为 `/var/tmp/rk3288-ffmpeg-kit-20261016`；结果与脱敏报告保存在项目 `cache/ffmpeg-kit-build-2026-10-16/`。原始ZIP及现有Windows NDK保持不变，未覆写Windows系统环境变量。

## 构建适配与命令

仅在缓存源码适配：禁用完整环境变量转储；ZIP构建标签使用已知哈希；限制6路编译；cpu-features添加 `-DBUILD_TESTING=OFF`，不下载无法在宿主执行的Android交叉测试；旧LTS JNI的 `MY_ARM_NEON := false` 改为true以满足NDK r25及本项目NEON要求。生产Gradle/CMake和Java推流调用未改。

主要上游命令：

```bash
bash android.sh --lts --api-level=21 --enable-gpl --enable-x264 \
  --enable-openssl --enable-android-zlib --disable-x86 --disable-x86-64 \
  --no-archive --no-link-time-optimization
```

JNI成功后，独立编译Java组件，保留原consumer ProGuard规则、资源与许可证，以ZIP组装自包含AAR。实际适配、参数、来源、收集与打包脚本均保留在上述缓存目录；未声称执行过上游Gradle的AAR任务，也未验证两次独立重建逐字节一致。

## 验证结果

| 验证 | 实际结果 |
|---|---|
| 三个原生变体及JNI | 均成功，最终构建退出码0 |
| ELF | ARM32 / AArch64正确；26库动态依赖闭合；无TEXTREL；未混入系统桩库 |
| 编码与协议配置 | libx264、OpenSSL、RTMP、RTMPS、TCP、TLS、FLV muxer、rawvideo demuxer均启用，二进制构建参数吻合 |
| Java与JNI接口 | 45个Java8类齐全；项目反射调用签名与JNI导出核验通过 |
| 项目APK组装 | 离线 `assembleDebug` 成功，真实AAR参与资源、DEX与原生库合并 |
| Android单测 | 401项，0失败、0错误、0跳过 |
| lint与模型门禁 | `lintDebug`、`checkApkFaceModels` 通过 |
| APK实际内容 | 26个目标原生库及FFmpegKit/smart-exception必需类均存在；无 `assets/face_models/` |

项目验证命令：

```powershell
.\gradlew.bat --no-daemon --offline -PRK_SKIP_OPENCV=true `
  :app:assembleDebug :app:testDebugUnitTest :app:lintDebug :app:checkApkFaceModels
```

该APK验证使用核心JNI桩；FFmpegKit的26个库为本次真实编译件。静态能力、组装和单测不等于Android设备加载、TLS握手、相机帧编码或实际推流通过。本轮没有真机、私有媒体或外网推流授权，也没有执行这些动作。

关键证据：`native-console-neon-jni.log`、`build-inputs.json`、`artifact-verification.json`、`native-output/capabilities.json`、`java/verification.json`、`android-integration.log`、`apk-verification.json`。前期GnuTLS链尝试未采用，原始失败记录保留，不计入成品；最终TLS后端为OpenSSL。

`.gitignore`仅新增成品AAR的精确忽略规则。回滚本机集成可将该AAR移回缓存，现有Gradle缺件分支即恢复禁用推流；相关来源登记应同步恢复。不重置既有Web整改，不清理来源不明的进程或其他缓存。

反思：范围扩展到必要的WSL工具安装、自包含Java依赖与APK组装验证。关键问题为挂载盘I/O、WSL网络、源码ZIP版本检查和旧LTS非NEON配置；分别通过持久工作目录、固定提交本地镜像、哈希标签及限定目标的JNI适配解决。

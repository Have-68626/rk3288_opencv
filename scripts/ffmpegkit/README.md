<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-19
-->

# FFmpegKit 离线重建工具

从用户提供的6.0.LTS源码ZIP及本地固定Git镜像，重建本项目需要的 **API21、ARMv7 NEON / ARM64、x264/GPL、OpenSSL** AAR。Windows可准备源码、编译Java和打包；原生交叉编译沿用既有WSL宿主与NDK **25.2.9519653**。不安装依赖，不自动联网补源码，不新增Linux产品目标。

消费位置仍为 `app/libs/ffmpeg-kit.aar`；新候选输出到 `cache/builds/`，确认后再单独集成。输出必须不存在，不覆盖旧证据或消费件。静态成功不代表设备加载、编码、RTMP会话或真实画面质量通过。

## 固定输入

[sources.lock.json](sources.lock.json)记录主ZIP摘要、5个依赖提交、smart-exception两个源码JAR摘要、ABI与NDK版本。主ZIP仅绑定用户提供的字节，未声称与远端归档逐字节认证。

本地mirror JSON示例；路径改成当前宿主可访问的绝对路径，Windows路径不能直接交给WSL：

```json
{
  "ffmpeg": "/absolute/dependency-sources/ffmpeg",
  "x264": "/absolute/dependency-sources/x264",
  "cpu-features": "/absolute/dependency-sources/cpu-features",
  "openssl": "/absolute/dependency-sources/openssl",
  "gnu-config": "/absolute/dependency-sources/gnu-config/805517123cbf"
}
```

GNU config使用 `v20210814` / `805517123cbfe33d17c989a18e78c5789fab0437`，也是上游前置输入。当前固定输入位置：

- 主源码ZIP：`cache/ffmpeg-kit-6.0.LTS.zip`。
- FFmpeg、x264、cpu-features、OpenSSL独立裸镜像：`cache/dependency-sources/ffmpegkit-6.0-lts/<依赖>/<提交前缀>/`；不使用旧仓alternates或硬链接。`mirrors.windows.json`和`mirrors.wsl.json`分别提供本机及WSL路径。
- GNU config：`cache/dependency-sources/gnu-config/805517123cbf/`。
- smart-exception：`cache/dependency-sources/smart-exception/0.2.1/`下两份sources JAR及`LICENSE.txt`。
- NDK离线包：`cache/tools/android-ndk/android-ndk-r25c-linux.zip`；使用前按所选临时路径解压，版本须为25.2.9519653。

这些输入已从旧混合构建目录独立保留并核对提交或摘要。旧`cache/ffmpeg-kit-build-2026-10-16/`及原始历史材料按[专批退出记录](../../docs/audits/2026-10-19-legacy-build-retirement-result.md)处理，不再作为重建前提。保留输入链不等于本轮重新编译AAR；消费件仍由`app/libs/ffmpeg-kit.aar`提供。

## 顺序执行

以下在仓库根执行；变量均指向现有输入或新的独占输出，不能复用已存在的构建目录：

```bash
python3 scripts/ffmpegkit/prepare-native.py \
  --source-zip "$source_zip" --mirrors "$mirrors_json" \
  --output "$prepare_output" --jobs 6
source_dir="$prepare_output/ffmpeg-kit-6.0.LTS"
consumer_rules="$source_dir/android/ffmpeg-kit-android-lib/consumer-rules.pro"

bash scripts/ffmpegkit/run-native.sh \
  --source-dir "$source_dir" --sdk-root "$sdk_root" --ndk-root "$ndk_root"

python3 scripts/ffmpegkit/collect-native.py \
  --source-dir "$source_dir" --ndk-root "$ndk_root" --output "$native_output"

python3 scripts/ffmpegkit/build-java.py \
  --source-dir "$source_dir" --common-sources "$smart_common_sources" \
  --java-sources "$smart_java_sources" --android-jar "$android_jar" \
  --javac "$javac" --output "$java_output"

python3 scripts/ffmpegkit/bundle-aar.py \
  --native-libs "$native_output/libs" --resources "$native_output/res" \
  --platform-libraries "$native_output/platform-libraries.json" \
  --classes-jar "$java_output/classes.jar" --consumer-rules "$consumer_rules" \
  --licenses "$smart_exception_license" \
  --output "$candidate_aar" --report "$bundle_report"
```

Java使用显式Android引导类库，生成class major 52。来源JAR须为smart-exception的common/java **sources**，不能传旧classes.jar。原生收集按API21平台库验证DT_NEEDED闭包、ABI、TEXTREL、系统桩，并分别检查两ABI的avcodec/avformat编译选项。打包保留原生res许可和显式传入的smart-exception许可；consumer rules使用原源码Android库的规则文件，不可省略。

WSL宿主建议将 `prepare_output` 放在其临时原生文件系统，避免在 `/mnt/d` 等跨盘目录进行大量配置/编译元数据访问；本轮实测遇到Perl等待 `p9_client_rpc`。`native_output`、`java_output`、候选AAR和报告仍收回项目 `cache/builds/`。删除临时源码前须完成Java构建、收集、打包，并保全consumer rules、固定输入清单、许可和结果摘要。

原生入口使用最小环境并移除上游环境转储；输出脱敏后透传，保留失败退出码。调用方应落盘脱敏日志并显示进程心跳。失败后不要覆盖输出重试，改用新输出名；保留错误摘要后，再将已停止使用的失败中间件列入清理清单。

## 校验边界

```powershell
python -m pytest tests/scripts/test_ffmpegkit_build_tools.py -q
```

Windows junction用例须实际创建重解析点；沙箱阻止创建时不能当作“已验证拒绝”。`prepare-native.py --verify-only --source-dir <目录>`仅核对记录的补丁和依赖HEAD，不证明未登记文件无变化，也不替代构建和输出摘要。

错误码区分来源摘要、补丁漂移、依赖提交、ABI/依赖/TEXTREL、Java版本和输出冲突。失败不打印完整子进程环境或stderr。更新消费AAR后须重新构建APK并绑定新原生代码；不能把旧APK实测转记为新候选验收。

存放见[仓库存储布局](../../docs/guides/repository-storage-layout.md)，历史消费件来源见[构建记录](../../docs/plans/2026-10-16-ffmpeg-kit-build.md)。

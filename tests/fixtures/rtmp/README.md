<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-15
-->

# RTMP 合成边界测试

执行：

```powershell
node --test tests/scripts/android-rtmp-behavior.test.js tests/scripts/rtmp-jni-contract.test.js
node scripts/check-rtmp-mutations.js
```

需要本机 JDK；优先使用 `JAVA_HOME`，其次查找 `PATH` 中的 JDK。测试不下载依赖，不访问外部媒体或推流服务。

行为测试编译当前生产 `FfmpegRtmpPusher`、`RtmpCameraInput`，并直接提取 `MainActivity` 的 RTMP 方法。固定件只替换 FFmpegKit、Android 和 JNI 边界，执行实际回环套接字传输，覆盖会话返回码、取消与重启竞争、凭据不回显、真实帧接线、低帧率补帧、无帧超时和资源释放。JNI 契约测试额外核对真实入口、注册表与无 OpenCV 降级桩。

FFmpegKit 固定件遵循[官方 Android API](https://arthenica.github.io/ffmpeg-kit/android/javadoc/com/arthenica/ffmpegkit/FFmpegKit.html)的 `executeWithArgumentsAsync(String[], FFmpegSessionCompleteCallback)` 签名，不提供旧代码误用的 `executeAsync(String[])`。

相机流为 640×480、15 FPS、RGBA 输入，经 H.264 编码输出 FLV；内容是处理后的画面，包含当前叠加，无音频。原始帧仅通过本机回环接口 `127.0.0.1` 的随机端口传递，独立 JNI 序列避免抢占 Bitmap 预览帧。

这些测试不能替代实际 FFmpegKit AAR 的编解码、Android 设备性能或服务端兼容性验证。AAR 缺失、编码器缺失、会话执行失败均须返回失败，不能报告推流成功。

#include <jni.h>
#include <android/log.h>

#define TAG "RK3288_JNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ═══════════════════════════════════════════════════════════════════
// Q-CQ-B07-008 边界声明：本文件是 RK_SKIP_OPENCV=ON 时的 native-lib 降级
// stub，职责仅限「JNI 描述符可解析 + 最小占位」。它不实现任何真实算法逻辑
// （检测/识别/推流等），不得被当作完整 native 使用。
//   - 每个 Java native 声明必须有对应的导出符号（JNI mangled 名），
//     保证运行时不会 UnsatisfiedLinkError；
//   - 返回类型按 Java 声明返回默认值：void 空操作、jboolean 返回 JNI_FALSE、
//     jstring 返回 stub 占位串；
//   - 真实 JNI 实现见 native-lib.cpp + jni/*.cpp（RK_SKIP_OPENCV=OFF 启用）。
// ═══════════════════════════════════════════════════════════════════

static void throwIllegalStateException(JNIEnv* env, const char* msg) {
    jclass exClass = env->FindClass("java/lang/IllegalStateException");
    if (exClass != nullptr) {
        env->ThrowNew(exClass, msg);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeInit(JNIEnv* env, jobject /* this */, jstring, jstring, jint) {
    LOGE("OpenCV disabled by RK_SKIP_OPENCV");
    throwIllegalStateException(env, "nativeInit failed: OpenCV disabled by RK_SKIP_OPENCV");
    return JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_MainActivity_stringFromJNI(JNIEnv* env, jobject /* this */) {
    return env->NewStringUTF("Hello from C++ (RK_SKIP_OPENCV=ON)");
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetFlip(JNIEnv*, jobject, jboolean, jboolean) {
}

// nativeGetFlipGeneration()J —— 任务3 诊断：占位返回 0（真实构建由 engine.cpp 注册返回 flipGeneration_）
extern "C" JNIEXPORT jlong JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetFlipGeneration(JNIEnv*, jobject) {
    return 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameYuv420888(JNIEnv* env, jobject /* this */, jobject, jint, jobject, jint, jint, jobject, jint, jint, jint, jint, jlong, jint, jboolean) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameYuv420888Bytes(JNIEnv* env, jobject /* this */, jbyteArray, jint, jbyteArray, jint, jint, jbyteArray, jint, jint, jint, jint, jlong, jint, jboolean) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetFrame(JNIEnv* env, jobject /* this */, jobject) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetPreviewSurface(JNIEnv* env, jobject /* this */, jobject) {
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeRenderFrameToSurface(JNIEnv* /* env */, jobject /* this */) {
    return JNI_FALSE;
}

// ── MainActivity 缺失描述符补齐（Q-CQ-B07-008：仅最小占位，返回默认值） ──

// nativeInitFile(String, String, String)Z —— 与 nativeInit 对应：占位返回 JNI_FALSE
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeInitFile(JNIEnv* /* env */, jobject /* this */, jstring, jstring, jstring) {
    return JNI_FALSE;
}

// nativeStart()V —— stub 占位空操作（真实实现会启动引擎线程）
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeStart(JNIEnv* /* env */, jobject /* this */) {
}

// nativeStop()V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeStop(JNIEnv* /* env */, jobject /* this */) {
}

// nativeRequestCancelInit()V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeRequestCancelInit(JNIEnv* /* env */, jobject /* this */) {
}

// nativeSetMode(I)V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetMode(JNIEnv* /* env */, jobject /* this */, jint) {
}

// nativeConfigureExternalInput(ZII)V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeConfigureExternalInput(JNIEnv* /* env */, jobject /* this */, jboolean, jint, jint) {
}

// ── NativeBridge 静态方法补齐（Q-CQ-B07-008：仅最小占位，返回默认值） ──
// 注意：static native → JNI 第二参数为 jclass（而非 jobject）

// nativeConfigureLog(String, String, String)V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_NativeBridge_nativeConfigureLog(JNIEnv* /* env */, jclass /* clazz */, jstring, jstring, jstring) {
}

// nativeInferFaceFromImage(...)Ljava/lang/String; —— stub 返回占位 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeBridge_nativeInferFaceFromImage(JNIEnv* env, jclass /* clazz */, jstring, jstring, jstring, jstring, jint, jfloat, jboolean, jboolean) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// nativeSetInferenceThrottle(String, I)V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_NativeBridge_nativeSetInferenceThrottle(JNIEnv* /* env */, jclass /* clazz */, jstring, jint) {
}

// nativeSetDetectionThrottle(String, I)V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_NativeBridge_nativeSetDetectionThrottle(JNIEnv* /* env */, jclass /* clazz */, jstring, jint) {
}

// nativeSetRecognitionThrottle(String, I)V —— stub 占位空操作
extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_NativeBridge_nativeSetRecognitionThrottle(JNIEnv* /* env */, jclass /* clazz */, jstring, jint) {
}

// ── NativeFaceBridge 静态方法补齐（任务13）──
// Q-CQ-B07-008：stub 无 JNI_OnLoad/RegisterNatives，NativeFaceBridge 的 native
// 方法只能靠 JNI mangled 符号解析；缺失即 UnsatisfiedLinkError。此处仅最小占位，
// 返回失败 JSON / null，不实现任何真实算法逻辑。
// 描述符与 src/cpp/jni/native_face_bridge.cpp 的 registerFaceBridgeMethods 逐项一致。
// 注意：static native → JNI 第二参数为 jclass（而非 jobject）。

// loadProfile(String, String)Ljava/lang/String; —— stub 返回占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_loadProfile(JNIEnv* env, jclass /* clazz */, jstring, jstring) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// activateRuntime(String, String, String, String)Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_activateRuntime(JNIEnv* env, jclass /* clazz */, jstring, jstring, jstring, jstring) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// replaceGallery([Ljava/lang/String;[Ljava/nio/ByteBuffer;)Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_replaceGallery(JNIEnv* env, jclass /* clazz */, jobjectArray, jobjectArray) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// beginEnrollment(String, ByteBuffer)Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_beginEnrollment(JNIEnv* env, jclass /* clazz */, jstring, jobject) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// cancelEnrollment()Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_cancelEnrollment(JNIEnv* env, jclass /* clazz */) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// getEnrollmentStatus()Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_getEnrollmentStatus(JNIEnv* env, jclass /* clazz */) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// takeEnrollmentTemplate()[B —— 无暂存模板，占位返回 null
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_takeEnrollmentTemplate(JNIEnv* /* env */, jclass /* clazz */) {
    return nullptr;
}

// #1 startEnrollmentCapture(ByteBuffer)Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_startEnrollmentCapture(JNIEnv* env, jclass /* clazz */, jobject) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// #1 stopEnrollmentCapture()Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_stopEnrollmentCapture(JNIEnv* env, jclass /* clazz */) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}

// getRuntimeStatus()Ljava/lang/String; —— 占位失败 JSON
extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_NativeFaceBridge_getRuntimeStatus(JNIEnv* env, jclass /* clazz */) {
    return env->NewStringUTF("{\"ok\":false,\"stub\":true}");
}
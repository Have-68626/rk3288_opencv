#include <jni.h>
#include <android/log.h>

#define TAG "RK3288_JNI"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

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

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeRelease(JNIEnv* env, jobject /* this */) {
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_MainActivity_stringFromJNI(JNIEnv* env, jobject /* this */) {
    return env->NewStringUTF("Hello from C++ (RK_SKIP_OPENCV=ON)");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetProfile(JNIEnv* env, jobject /* this */) {
    return env->NewStringUTF("{}");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeUpdateConfig(JNIEnv* env, jobject /* this */, jstring) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeClearDatabase(JNIEnv* env, jobject /* this */) {
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetEnrollMode(JNIEnv* env, jobject /* this */, jboolean) {
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetIdentityId(JNIEnv* env, jobject /* this */, jint) {
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetDbState(JNIEnv*, jobject, jintArray) {
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeGetMetrics(JNIEnv* env, jobject /* this */, jlongArray) {
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetFlip(JNIEnv*, jobject, jboolean, jboolean) {
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativeSetDisplayThreshold(JNIEnv* env, jobject /* this */, jfloat) {
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameBytes(JNIEnv* env, jobject /* this */, jbyteArray, jint, jint, jint, jlong, jint, jboolean) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameYuv420888(JNIEnv* env, jobject /* this */, jobject, jint, jobject, jint, jint, jobject, jint, jint, jint, jint, jlong, jint, jboolean) {
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_rk3288_1opencv_MainActivity_nativePushFrameNV21Bytes(JNIEnv* env, jobject /* this */, jbyteArray, jint, jint, jint, jlong, jint, jboolean) {
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

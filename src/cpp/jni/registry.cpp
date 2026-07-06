/**
 * @file jni/registry.cpp
 * @brief JNI 方法注册中心
 *
 * 汇聚各领域的 JNI 方法，通过 JniMethodRegistry 统一注册到对应 Java 类。
 * 支持多类注册（MainActivity 实例方法 + NativeBridge 静态方法）。
 */
#include <jni.h>

#include "JniMethodRegistry.h"
#include "jni_shared.h"

jint registerAllNativeMethods(JNIEnv* env) {
    // 注册 MainActivity 实例方法
    JniMethodRegistry mainRegistry;
    registerCameraMethods(mainRegistry);
    registerEngineMethods(mainRegistry);
    registerPreviewMethods(mainRegistry);

    jclass mainClazz = env->FindClass("com/example/rk3288_opencv/MainActivity");
    if (!mainClazz) {
        return JNI_ERR;
    }
    jint count = mainRegistry.registerAll(env, mainClazz);
    env->DeleteLocalRef(mainClazz);
    if (count < 0) {
        return JNI_ERR;
    }

    // 注册 NativeBridge 静态方法
    JniMethodRegistry bridgeRegistry;
    registerConfigMethods(bridgeRegistry);

    jclass bridgeClazz = env->FindClass("com/example/rk3288_opencv/NativeBridge");
    if (!bridgeClazz) {
        return JNI_ERR;
    }
    jint count2 = bridgeRegistry.registerAll(env, bridgeClazz);
    env->DeleteLocalRef(bridgeClazz);
    if (count2 < 0) {
        return JNI_ERR;
    }

    return count + count2;
}

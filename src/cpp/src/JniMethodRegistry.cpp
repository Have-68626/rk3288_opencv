#include "../include/JniMethodRegistry.h"

void JniMethodRegistry::add(std::initializer_list<JniMethodDef> methods) {
    methods_.reserve(methods_.size() + methods.size());
    for (const auto& m : methods) {
        methods_.push_back(m);
    }
}

jint JniMethodRegistry::registerAll(JNIEnv* env, jclass clazz) {
    if (methods_.empty()) return 0;

    std::vector<JNINativeMethod> jniMethods;
    jniMethods.reserve(methods_.size());
    for (const auto& m : methods_) {
        jniMethods.push_back({const_cast<char*>(m.name),
                              const_cast<char*>(m.signature),
                              m.fnPtr});
    }

    if (env->RegisterNatives(clazz, jniMethods.data(),
                             static_cast<jint>(methods_.size())) != JNI_OK) {
        return -1;
    }
    return static_cast<jint>(methods_.size());
}

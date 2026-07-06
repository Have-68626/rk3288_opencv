#pragma once

#include <vector>
#include <jni.h>

struct JniMethodDef {
    const char* name;
    const char* signature;
    void* fnPtr;
};

class JniMethodRegistry {
public:
    void add(std::initializer_list<JniMethodDef> methods);
    jint registerAll(JNIEnv* env, jclass clazz);
    std::size_t count() const { return methods_.size(); }

private:
    std::vector<JniMethodDef> methods_;
};

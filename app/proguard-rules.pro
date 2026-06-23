# ProGuard rules for RK3288 AI Engine
# Keep JNI methods and native entry points
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.example.rk3288_opencv.** { *; }

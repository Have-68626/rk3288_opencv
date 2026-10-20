<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-19
-->

# 辅助工具入口

工具按职责组织，既有通用/兼容命令保留根入口。构建命令统一见[构建与测试](../docs/guides/build-and-test.md)，模型身份见[模型消费者](../docs/guides/model-storage-and-consumers.md)。

| 位置 | 职责 |
|---|---|
| scripts根 | 仓库门禁、量化入口及已有Windows/设备验证命令；被CI固定调用的名称保留 |
| android/ | 设备状态、运行采样、离线证据与APK检查，说明见Android验证指南 |
| audit/ | 审计映射、评分、展示，具体边界见本目录README |
| ffmpegkit/ | 固定来源的AAR重建链，具体阶段与消费边界见本目录README |
| src/cpp/tools/ | 需CMake编译的独立工具源码，包括ncnn_calibration_images |
| tests/fixtures/opencv/ | 不含媒体/身份数据的公共参数清单 |
| tests/scripts/ | 行为回归、命令替身和契约检查；原始运行日志不放此处 |

## Windows批处理约定

三个构建/验证入口从脚本位置定位仓库，CMake显式使用-S/-B，源码与输出不依赖调用终端的当前目录。脚本以UTF-8和CRLF保存；根gitattributes只为scripts/*.bat设置此例外。

- build_android.bat：独立Android验证CLI树cache/builds/android-opencv-ninja-armeabi-v7a；不是Gradle APK入口。
- verify_opencv_host.bat：Host CLI树cache/builds/windows-opencv-vs2022-x64，报告cache/reports/opencv-host；检查真实退出码后再检查SUMMARY标记。
- verify_faces_test_set01.bat：共用上述Host CLI树，默认报告cache/reports/test-set01；显式相对输入/输出路径仍相对调用终端，默认路径相对仓库。
- verify_face_models.py：独立模型身份校验，默认报告cache/reports/face-models，可显式指定--report。

真实图像验证需操作者准备并授权数据；本轮自动回归仅使用合成命令/图片夹具，不读取现有私有图片或识别日志。工具输出不自动入库，也不以忽略规则推定可以删除。

固定可复用产物目录不代表跨工具链兼容：更换生成器、ABI、CRT或后端须使用新的构建树，不能直接搬旧CMake目录。新源码、回归和发现说明入库；旧缓存原件按来源/消费者退出条件单独处理。

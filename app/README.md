<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-19
-->

# Android模块入口

本目录维护Gradle装配、manifest/res、可选推流AAR及工具托管的构建输出。生产Java位于src/java，Java单测位于tests/unit/java，由build.gradle的sourceSets显式指定；app/src/test不是当前测试入口，勿在该默认目录新增测试后误认为已执行。

构建、Java/NDK/ABI及E2E以[统一指南](../docs/guides/build-and-test.md#android)为准，模型来源与导入见[模型存储和消费者](../docs/guides/model-storage-and-consumers.md)。app/libs/ffmpeg-kit.aar是既有可选消费入口，不随本轮构建候选替换。

app/build与app/.cxx保留AGP默认布局；本地审计可用明确初始化脚本将本次重建放独立cache/builds，但不要直接移动已有树。APK无内置模型门禁继续生效，结构与mock测试不能代替真实后端或设备验收。

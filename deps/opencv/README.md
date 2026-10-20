# OpenCV核心测试桩

本目录仅供显式RK_SKIP_OPENCV=ON的核心测试使用，包含占位头文件与构建目标；不是完整OpenCV源码、安装包或真实图像推理实现。

真实Windows/Android构建必须将OPENCV_ROOT指向外部完整OpenCV源码，并按目标提供opencv_contrib。不要将本目录当作可部署依赖或真实精度验收依据。构建约束见../../AGENTS.md及../../docs/guides/build-and-test.md。

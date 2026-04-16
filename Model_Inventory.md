# 模型台账 (Model Inventory)

本项目使用或支持以下机器视觉模型。对于需要在部署时下载的模型，请确保计算其 SHA-256 哈希值与下表一致，以确保安全性与精度。

| 模型名称 | 用途 | 格式 | 仓库/部署路径 | 来源 / 下载地址 | SHA-256 Hash |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **LBP Frontal Face** | 传统人脸检测（级联分类器） | XML | `app/src/main/assets/lbpcascade_frontalface.xml`<br>*(代码库内置)* | [OpenCV Data](https://github.com/opencv/opencv/tree/master/data/lbpcascades) | `529f217132809f287aaed5cd35dc00d9bc9b2afebe46dd1fe90ecb67f1daad0d` |
| **ResNet SSD Face Detector (8-bit)** | 高精度人脸检测 (DNN) | PB | `storage/models/opencv_face_detector_uint8.pb`<br>*(部署时手动下载)* | [OpenCV 3rdparty](https://github.com/opencv/opencv_3rdparty/raw/dnn_samples_face_detector_20180205_fp16/res10_300x300_ssd_iter_140000_fp16.caffemodel) | *(按实际下载版本定)* |
| **ResNet SSD Config** | 配合上述 DNN 模型的网络结构定义 | PBTXT | `storage/models/opencv_face_detector.pbtxt`<br>*(部署时手动下载)* | [OpenCV Extra](https://raw.githubusercontent.com/opencv/opencv_extra/master/testdata/dnn/opencv_face_detector.pbtxt) | *(按实际下载版本定)* |
| **NCNN YOLO Face (Bin)** | 端侧高精度人脸检测 (NCNN) | BIN | *(按需配置路径)* | 第三方提供或自训练 | *(按实际版本定)* |
| **NCNN YOLO Face (Param)** | NCNN 网络结构定义 | PARAM | *(按需配置路径)* | 第三方提供或自训练 | *(按实际版本定)* |
| **ArcFace Embedder** | 人脸特征提取 (NCNN/ONNX) | ONNX / BIN | *(按需配置路径)* | 第三方提供或自训练 | *(按实际版本定)* |

> **提示：** 启动时程序将自动进行自检，并会在日志中打印所有加载模型的 SHA-256。请确保与上述台账保持一致。如果在模型加载阶段发现缺失，请根据“来源 / 下载地址”获取对应的模型并放置在正确路径。

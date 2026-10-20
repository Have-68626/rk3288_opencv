<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-19
-->

# 模型存储与消费者

本页维护目录角色和消费者导航，不复制模型摘要、许可证或预处理权威值。父链与许可见[供应链账本](../security/supply-chain-ledger.md)，转换步骤见[INT8操作](int8-quantization.md)，历史移动见[资产映射](../audits/assets/2026-10-18-storage/asset-paths.json)。

## 目录职责

| 路径 | 角色 | 当前消费者与保留条件 |
|---|---|---|
| deps/insightface/*.onnx | 上游父图 | 转换输入；来源和SHA以供应链为准，不以mtime证明父链 |
| deps/insightface/archives、optional | 原发行包及用途待确认输入 | 离线来源保全，不把识别器相同当成整包可删 |
| deps/WIDER_train | 校准/评估数据源 | 量化或精度任务显式指定；涉及媒体授权，不随路径检查读取 |
| cache/model-conversion/insightface | 新任务转换中间件的约定位置 | 历史副本经专批退出；新输出须绑定本次父图和工具身份，不得直接当作已认证生产消费件 |
| models/calib_images | 历史校准副本 | 独立精度程序的历史默认位置；不是生产模型目录，不因结构测试通过获准清理 |
| models/scrfd_face_detect_ncnn | Android当前检测源模型 | Gradle源文件表与独立校验器共同消费，profile确定具体变体 |
| models/arcface_ncnn、arcface_int8_ncnn | Android当前识别源模型及其他工具候选 | profile源文件表决定Android消费件；同族不同精度不能只凭文件名混用 |
| models/android/<profile> | Android导入契约文本 | manifest、LICENSE、NOTICE入库；其相对路径是包内路径，不是仓库源文件路径 |
| models/mobilefacenet_ncnn、mobilefacenet_int8_ncnn | 轻量识别图和历史/可选注册入口 | 是否生效按配置和ModelRegistry判定；不能因目录存在宣称当前Android profile使用它 |
| models/int8 | 指定任务输出/消费候选 | 部分历史量化或校验工具使用；与分族目录并存，不作为全项目默认别名 |
| models/scrfd_ncnn → models/scrfd_int8_ncnn | CI量化配置约定 | 当前workflow指向这些路径；本机不保证存在，不自动从其他SCRFD目录复制替代 |
| models/yolo_face_int8_ncnn | 历史YOLO检测注册与结构测试入口 | 与SCRFD是不同解码/注册语义；不得为统一命名而互设别名或互拷模型 |

## 当前Android源路径如何确定

从[Gradle](../../app/build.gradle)的faceModelSources表和[独立校验器](../../scripts/verify_face_models.py)的PROFILES定位仓库源文件，再对照对应manifest中的包内路径和SHA。表项变更须同步二者及manifest；已有face-model-table-consistency回归负责核对这组接口。

rk3288_light与arm64_accuracy使用各自表项，不根据profile名称推断检测器大小、识别器家族或精度。SHA相等只证明字节身份，不证明真实人脸精度或商用许可。模型空间/旧模板约束以[Android注册设计](../designs/android-enrollment-design.md)为准。

## 后续产物与迁移

- 新转换在cache/model-conversion内使用明确模型族和本次标识；量化输入保持唯一同stem配对，不覆盖正式消费目录。
- 消费者优先读取明确profile/配置，不通过“发现哪个文件夹就启用哪个模型”扩张能力。
- 先在独立输出验证父图、节点、ABI、预处理、四文件摘要和templateSpaceId，再单独批准切换正式消费件；本页登记不替代该批准。
- 历史asset-paths记录当时移动，不追加本轮未实际移动的条目；新用途视图由本页维护。正式模型、校准集、父图和发行包保留原位；旧cache转换中间件按[专批退出记录](../audits/2026-10-19-legacy-build-retirement-result.md)处理，不能再以历史映射推定本机副本仍存在。

校验命令从仓库根执行：`python scripts/verify_face_models.py`。模型身份报告仅留本机cache/reports/face-models，不把该缓存作为正式说明的稳定引用；不运行校准图片或真实精度任务。

# 人脸识别技术实现方案研究

本研究报告提供人脸识别方案的工程化对比与落地模板，强调“口径一致”“可复现评估”“合规优先”“可维护集成”。

### 6.1 端侧离线 vs 云端：原理、模板、阈值口径、延迟与隐私差异

<a id="tbl-6-1"></a>
#### 表 6-1 离线端侧与云端方案对比表（工程视角）
| 维度 | 离线端侧（设备本地） | 云端（服务端 API） |
| :--- | :--- | :--- |
| 网络依赖 | 无 | 强依赖 |
| 延迟组成 | 预处理 + 推理 + 1:N 检索 | 预处理 + 上传 + 服务推理 + 返回 |
| 数据合规 | 生物特征不出端更易满足最小化原则 | 需额外合规审查与传输/存储说明 |
| 成本 | 设备算力与本地存储成本 | API 调用成本与带宽成本 |
| 可控性 | 高（阈值、版本、回滚） | 中（受服务更新影响） |

#### 6.1.1 常见端侧/云端方案快速对比（结论导向）
本表强调“能否在 RK3288 工控量产场景可控落地”，不追求覆盖全部能力项；最终仍需以本项目基准口径（6.3/6.4）做实测裁决。

<a id="tbl-6-6"></a>
#### 表 6-6 人脸方案对比（ML Kit / MediaPipe / ArcFace / Dlib / 百度 / 优图）
| 方案 | 类型 | 典型输出能力 | 集成复杂度 | RK3288 风险点 | 成本/许可 | 推荐落地形态 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Google ML Kit（Face Detection） | 端侧 | 人脸框/关键点/追踪（偏检测） | 低到中 | 算子与机型差异需实测；包体与性能需评估 | 免费（以官方条款为准） | 用于“检测/质量分/关键点”；识别（特征）建议自研/自带模型 |
| MediaPipe（Face Detector / Face Landmarker） | 端侧 | 人脸框/关键点/FaceMesh（偏几何） | 中 | 构建体系与依赖版本需固定；部分方案对 GPU/NNAPI 依赖需评估 | 免费（以官方条款为准） | 用于“关键点/对齐/质量评估”；识别向量建议独立模型 |
| ArcFace（算法/论文；常见开源实现为 InsightFace 等） | 端侧 | 以“特征提取（Embedding）+ 余弦相似度”为核心的识别链路 | 中 | 需要自行补齐检测/对齐/存储/检索与阈值口径；模型转换与端侧加速需评估 | 开源（以具体实现许可证为准） | 推荐“自研可控链路”：检测/对齐可独立，特征模型固定并版本化 |
| 虹软 ArcFace（商业 SDK/商标产品） | 端侧 | 检测 + 特征提取 + 1:1/1:N（取决于 SDK 形态） | 中到高 | 许可证绑定、ABI/so 兼容、离线激活与设备更换流程 | 商业授权 | 适合量产：需把激活/版本/阈值/回滚做成可审计闭环 |
| Dlib | 端侧 | HOG/CNN 检测 + 128D 特征（经典方案） | 高 | NDK 编译复杂、体积大、性能/NEON 优化不确定 | 开源许可（以官方为准） | 更适合研究/验证；量产不推荐作为主链路 |
| 百度 AI 人脸（Face） | 云端 | 检测/对比/检索/活体（按产品线） | 中 | 网络抖动与延迟；密钥管理；服务变更 | 按量计费 | 推荐“自建后端转发 + 端侧最小上传 + 全链路审计” |
| 腾讯优图/腾讯云人脸 | 云端 | 检测/对比/检索/活体（按产品线） | 中 | 同上；还需关注区域与合规条款 | 按量计费 | 推荐“自建后端转发”，端侧只拿业务结果与审计号 |

#### 6.1.2 主流云厂商补充对比（阿里 / AWS / Azure）
云厂商更适合“云端识别/跨设备共享库/集中审计”的业务形态；若目标是 RK3288 离线门禁，云方案应作为可选备援链路，而非主链路。

<a id="tbl-6-7"></a>
#### 表 6-7 云厂商对比（阿里 / AWS / Azure）
| 厂商 | 典型服务 | 典型能力 | 识别形态 | 合规/区域要点 | 计费与限额 | 推荐集成方式 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 阿里云 | Facebody（人脸人体） | 检测/对比/属性/活体（视产品线） | 1:1/检索（视产品线） | 需按业务地区选择地域与合规策略 | 按量计费，需关注 QPS/并发限制 | 仅后端调用：端侧→自建服务→阿里云（统一鉴权与审计） |
| AWS | Rekognition | 检测/对比/搜索/集合（Collection） | 1:1/1:N | 数据出境与地域合规需评审 | 按量计费，限额需预先申请提升 | 仅后端调用：端侧不直连 AWS，统一通过自建服务做签名与审计 |
| Azure | Face API（Cognitive Services） | 检测/对比/识别/相似度（以版本为准） | 1:1/1:N | 关注资源所在区域与数据保留策略 | 按量计费，限额与配额可配置 | 仅后端调用：端侧只传最小化数据并拿到审计号 |

### 6.2 Android 集成步骤模板（依赖/模型/混淆/ABI）

#### 6.2.1 依赖与资源布局（模板）
- 模型/权重：建议放置在 `app/src/main/assets/models/` 或按产品策略从安全通道拉取后落盘（路径由配置项决定，不写死）。
- ABI：仅保留目标 ABI（RK3288 多为 `armeabi-v7a`），减少包体。

```gradle
android {
    defaultConfig {
        ndk {
            abiFilters "armeabi-v7a"
        }
    }
}
```

#### 6.2.2 混淆与反射（最小模板）
```text
-keep class androidx.camera.** { *; }
-dontwarn androidx.camera.**
```

#### 6.2.3 人脸方案逐项集成清单（从“取帧”到“门禁事件”）
目标是把方案落地拆成可验收、可定位故障的最小闭环。每一项都必须定义输入/输出与可观测日志，避免“黑箱集成”。

<a id="tbl-6-4"></a>
#### 表 6-4 集成清单与交付物（建议作为里程碑）
| 模块 | 输入 | 输出 | 失败表现 | 最小验收口径 |
| :--- | :--- | :--- | :--- | :--- |
| 帧接入（CameraX/Camera2） | `YUV_420_888` | 统一的 RGB/灰度张量 | 黑屏/卡死/帧率不稳 | 可稳定跑 10 分钟，无泄漏上升 |
| 人脸检测 | 帧张量 | 人脸框/关键点/质量分 | 误检/漏检 | 在基准集上输出固定 JSON 记录 |
| 对齐与裁剪 | 人脸框/关键点 | 归一化人脸 | 特征漂移 | 关键点对齐误差可统计 |
| 特征提取（Embedding） | 归一化人脸 | 向量（维度固定） | 抖动/不稳定 | 同图多次提取余弦相似度≥0.999 |
| 1:N 检索 | 向量 + 库 | TopK + score | 误识/延迟高 | N≤10000 时 P95 < 150ms（口径见 6.3） |
| 阈值与策略 | TopK + score + 质量 | PASS/REJECT/RETRY | 误放/误拒 | 阈值、拒识策略版本化可回滚 |
| 活体检测（PAD） | 帧序列/人脸 ROI | LIVE/SPOOF + 置信度 | 被照片/屏幕攻破 | 指标口径（APCER/BPCER）固定（见 6.4） |
| 特征存储 | 向量/用户信息 | 加密文件/DB | 泄露/损坏 | AES-GCM 加密；可迁移/可清理 |
| 审计与门禁事件 | PASS/REJECT | 事件记录/截图/录像 | 无法追溯 | 事件含 build_id、阈值版本、PAD 结果 |

#### 6.2.4 端侧 SDK 集成模板（ML Kit / MediaPipe / ArcFace / Dlib）
端侧集成的共同目标是：把“黑箱 SDK”拆成可审计的输入/输出与日志口径，并把版本、阈值、模型与许可证流程纳入可回滚的发布体系。

ML Kit（检测）最小接入模板（Kotlin，来自 ImageAnalysis）：
```kotlin
import androidx.camera.core.ImageProxy
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.face.FaceDetection
import com.google.mlkit.vision.face.FaceDetectorOptions

class MlKitFaceDetector {
    private val detector = FaceDetection.getClient(
        FaceDetectorOptions.Builder()
            .setPerformanceMode(FaceDetectorOptions.PERFORMANCE_MODE_FAST)
            .enableTracking()
            .build()
    )

    fun detect(imageProxy: ImageProxy, onDone: (faceCount: Int) -> Unit, onError: (Throwable) -> Unit) {
        val mediaImage = imageProxy.image ?: run {
            imageProxy.close()
            onDone(0)
            return
        }
        val img = InputImage.fromMediaImage(mediaImage, imageProxy.imageInfo.rotationDegrees)
        detector.process(img)
            .addOnSuccessListener { faces -> onDone(faces.size) }
            .addOnFailureListener { e -> onError(e) }
            .addOnCompleteListener { imageProxy.close() }
    }
}
```

MediaPipe（关键点/对齐）接入要点（模板口径）：
- 固定版本：将依赖版本写死在 Gradle，并在文档与构建产物中记录（见 2.3 节“变动项修改入口”）。
- 线程与背压：只允许单一分析线程；输入队列必须可控（与 CameraX `KEEP_ONLY_LATEST` 对齐）。
- 输出口径：至少输出关键点数量、置信度、耗时（ms），并为每次推理输出可关联的 `frame_id`。

ArcFace（商业 SDK）接入要点（模板口径）：
- 许可证：禁止写死在 APK；通过安全通道下发或由设备侧安全区持有，落盘需加密并可吊销。
- ABI：按设备只打包 `armeabi-v7a`，并在启动自检时校验 so 是否齐全、版本是否匹配。
- 日志：必须输出 SDK 版本、激活状态、阈值版本、特征维度、失败错误码与重试次数（写入 `ErrorLog/`）。

Dlib（NDK）接入要点（模板口径）：
- 构建：建议单独形成 `docs/examples/` 可复现工程；不要把大段构建脚本散落在业务模块。
- 性能：必须在目标设备实测 P95 延迟；若无法满足 6.3 口径，则只保留为研究分支，不进入量产主链路。

<a id="tbl-6-8"></a>
#### 表 6-8 特征维度/模板大小/阈值/延迟对比表（统一口径模板，需以实测填充）
| 方案 | 特征维度（Embedding Dim） | 模板大小（单人，bytes） | 相似度度量 | 初始阈值建议（余弦） | 端侧 P95（特征提取 ms） | N=10000 P95（检索 ms） | 备注（许可/落地） |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| ArcFace（开源实现：InsightFace） | 常见 512（以模型为准） | 维度×4（float32）或 ×1（int8） | 余弦相似度 | 0.2~0.6（必须以数据集标定） | 待实测 | 待实测 | 强制版本化模型与阈值；量化会改变阈值分布 |
| 虹软 ArcFace（商业 SDK） | 以 SDK 文档为准 | 以 SDK 文档为准 | SDK 内部定义/通常可映射余弦 | 以 SDK 推荐为准 | 待实测 | 待实测 | 许可证/离线激活流程必须可审计可回滚 |
| Dlib（128D） | 128 | 128×4（float32） | 余弦/欧氏（以实现为准） | 需标定 | 待实测 | 待实测 | NDK 构建与性能风险高，默认不进量产主链路 |
| 云端（百度/优图等） | 厂商侧 | 厂商侧 | 厂商侧 | 厂商侧 | 网络决定 | 网络决定 | 端侧只保留最小上传与审计号，阈值口径需统一映射 |

#### 6.2.6 逐方案集成模板（保证“口径一致、可回滚、可观测”）
本小节提供“逐方案最小可交付模板”，每个模板都以同一条主链路拆分：取帧 → 检测 → 对齐 → 特征 → 检索 → 决策 → 审计。

##### 6.2.6.1 ArcFace（开源实现：InsightFace）集成模板（建议主链路形态）
- 输入：固定 `YUV_420_888` → 统一预处理（旋转/裁剪/对齐）→ 模型输入张量（固定尺寸与归一化）。
- 输出：固定维度向量（float32 或 int8），并记录 `embedding_dim`、`model_version`、`preprocess_version`。
- 阈值：必须与数据集/标注/统计脚本绑定版本（见 6.3 表 6-2），禁止只改代码不改口径。

##### 6.2.6.2 虹软 ArcFace（商业 SDK）集成模板（许可证闭环优先）
- 初始化自检：输出 SDK 版本、激活状态、ABI 匹配结果、特征维度、错误码映射表版本。
- 输入输出封装：把 SDK 的输入格式统一映射为“归一化人脸 ROI + 旋转角 + 时间戳”，输出统一为“向量/或 SDK token + score”。
- 灰度/红外：若设备存在 IR/TOF 链路，必须把“哪一路进入识别”做成配置项，并写入审计事件。

##### 6.2.6.3 MediaPipe（关键点/对齐）集成模板（作为对齐模块更稳）
- 角色定位：优先用于关键点/FaceMesh 与质量评估，把“检测/识别”拆开以降低耦合。
- 线程模型：单线程分析 + 明确背压；每帧必须输出 `frame_id` 与耗时，便于定位抖动来源。

##### 6.2.6.4 ML Kit（检测）集成模板（作为检测模块更稳）
- 角色定位：只承担“检测/追踪/质量评估”，输出框与关键点；识别向量不要依赖 ML Kit（避免口径漂移）。
- 异常处理：每次回调必须保证 `ImageProxy.close()`，否则会触发背压堆积（见 5.5.1）。

##### 6.2.6.5 Dlib（研究链路）集成模板（只做可复现验证）
- 交付边界：仅交付 `docs/examples/` 级别的可复现工程与基准 CSV，不与业务模块强耦合。
- 退出条件：若在 RK3288 设备上 P95 不能满足 6.3 口径，则标记为“研究保留”，不进入主链路。

#### 6.2.5 云端 API 集成模板（百度 / 优图 / 阿里 / AWS / Azure）
云端集成必须满足两条硬约束：端侧不直连云厂商（避免密钥泄露与难审计），以及全链路最小化上传（只上传业务必要字段，尽量在端侧裁剪/脱敏）。

推荐拓扑（模板）：
- Android：只负责采集/裁剪/压缩 + 业务状态机，不持有云密钥。
- 自建后端：统一鉴权、签名、配额、熔断、审计与缓存，按需转发到各云厂商。
- 云厂商：只接收后端请求，并返回标准化结果。

后端环境变量模板（示例命名，按你们实际改）：
```text
BAIDU_API_KEY=...
BAIDU_SECRET_KEY=...
TENCENT_SECRET_ID=...
TENCENT_SECRET_KEY=...
ALICLOUD_ACCESS_KEY_ID=...
ALICLOUD_ACCESS_KEY_SECRET=...
AWS_ACCESS_KEY_ID=...
AWS_SECRET_ACCESS_KEY=...
AZURE_FACE_ENDPOINT=...
AZURE_FACE_KEY=...
```

> 安全提示（必须遵守）：
> - 以上变量只允许出现在服务器环境变量/密钥管理系统/本机私有配置中，禁止写入 APK、禁止提交到 Git。
> - 端侧（Android/Windows 客户端）不得直连云厂商，也不得持有任何云密钥；只允许调用自建后端的受控接口。

端侧 → 自建后端（HTTP）最小请求模板（示例）：
```json
{
  "request_id": "uuid",
  "device_id": "masked",
  "image_jpeg_base64": "<base64>",
  "mode": "detect|verify|identify",
  "options": {
    "max_faces": 1,
    "return_landmarks": true
  }
}
```

自建后端统一输出模板（建议）：
```json
{
  "request_id": "uuid",
  "provider": "baidu|tencent|alicloud|aws|azure",
  "audit_id": "provider_trace_id",
  "latency_ms": 123,
  "faces": [
    {
      "bbox": [0, 0, 100, 100],
      "score": 0.98,
      "landmarks": []
    }
  ],
  "match": {
    "user_id": "masked",
    "score": 0.83,
    "threshold": 0.80,
    "decision": "PASS|REJECT|RETRY"
  }
}
```

### 6.3 硬件要求与 1:N 基准口径（≤10000，<150ms，≥97%）

#### 6.3.1 指标与记录格式（建议作为统一口径）
- 1:N：输入 1 张查询人脸，与 N 条已注册特征进行相似度检索并返回 TopK。
- 延迟：统计“预处理 + 特征提取 + 检索”的总耗时，输出 P50/P95。
- 识别率：按数据集与标注口径定义（TP/FP/FN、阈值、拒识策略必须固定）。

<a id="tbl-6-2"></a>
#### 表 6-2 基准结果记录格式（CSV 字段定义）
| 字段 | 含义 |
| :--- | :--- |
| device_id | 设备标识（机型/序列号脱敏） |
| build_id | 应用版本/构建号 |
| dataset_id | 数据集标识（如 test_set01） |
| n_gallery | 库大小 N |
| tt_feature_ms | 特征提取耗时（ms） |
| tt_search_ms | 检索耗时（ms） |
| tt_total_ms | 总耗时（ms） |
| threshold | 阈值 |
| top1_correct | Top1 是否正确（0/1） |

#### 6.3.2 基准执行流程（可复现/可追责）
建议将“数据集/配置/代码”同时版本化，避免口径漂移：
- 数据集：`dataset_id` 必须与“来源/清洗规则/标注版本/脱敏策略”一一对应。
- 配置：阈值、TopK、拒识策略、PAD 开关等写入一份可导出的配置快照（用于复现）。
- 记录：每次基准都产出 CSV（见表 6-2）+ 汇总报告（P50/P95、ROC/DET、混淆矩阵）。

#### 6.3.3 CI 门禁（两级门禁：PR 快速门禁 + 设备夜间门禁）
门禁目标不是“在 CI 上跑出最终识别率”，而是防止关键能力退化与口径漂移。

PR 快速门禁（必须通过，分钟级）：
- 构建门禁：assembleDebug + 基础单测通过。
- 兼容门禁：模型文件校验（hash/版本号），向量维度与序列化格式不变。
- 回归门禁：TopK 排序稳定、阈值策略单测覆盖、加解密存储可读可写。

设备夜间门禁（可选，小时级，建议自建 Runner + 真机）：
- 在目标 RK3288 设备上跑固定基准集，产出 CSV 与汇总报告并作为 CI artifact。
- 阈值/识别率/延迟三项同时设“红线”，超出即阻断合并或标红报警（示例默认红线：Top1≥97%，N≤10000 时 P95<150ms，PAD 的 APCER/BPCER 需在产品定义范围内）。

### 6.4 活体检测对比与攻击用例（关联 ISO 30107-3）

<a id="tbl-6-3"></a>
#### 表 6-3 活体检测路线对比（工程可交付）
| 路线 | 硬件依赖 | 抗攻击能力 | 代价 | 适用场景 |
| :--- | :--- | :--- | :--- | :--- |
| RGB 静默 | 无 | 低到中 | 低 | 低风险场景 |
| RGB 动作指令 | 无 | 中 | 中 | 人机交互可接受 |
| 红外（IR） | IR 摄像头 | 中到高 | 中到高 | 工业门禁/中风险 |
| 3D 结构光 | 专用模组 | 高 | 高 | 高风险认证 |

攻击用例建议最小集合：照片翻拍、屏幕播放视频、打印面具/硅胶面具、不同光照与角度干扰。

#### 6.4.1 PAD 指标口径（最小集合）
<a id="tbl-6-5"></a>
#### 表 6-5 PAD 指标字段与解释（ISO 30107-3 术语对齐）
| 指标 | 含义 | 备注 |
| :--- | :--- | :--- |
| APCER | 攻击样本被错误接受的比例 | 越低越好 |
| BPCER | 真用户被错误拒绝的比例 | 越低越好 |
| ACER | (APCER + BPCER)/2 | 汇总指标 |
| EER | 等错误率点 | 用于对比不同阈值 |

### 6.5 合规与性能优化：特征加密存储、热更新、多线程、零拷贝、降级策略

#### 6.5.1 特征加密存储（AES-256-GCM 口径）
本小节目标是提供一套“可直接拷贝使用”的 **AES-256-GCM + Android Keystore** 特征加密落盘模板，并明确异常处理与降级策略，避免把“存储加密”做成黑箱。

**安全目标（工程口径）**：
- 密钥仅存在于 Keystore，不可导出；应用卸载/清数据后自动失效。
- 文件被拷贝到其他设备/用户空间后不可解密（与设备 Keystore 绑定）。
- 任意篡改密文/IV/AAD 都必须解密失败（GCM 完整性校验）。
- 落盘文件不包含可反推生物特征的明文字段（例如 user_id 明文、向量维度明文等可选）。

<a id="tbl-6-9"></a>
#### 表 6-9 特征加密文件格式（建议固定，便于兼容与迁移）
| 字段 | 长度 | 含义 | 备注 |
| :--- | ---: | :--- | :--- |
| magic | 4 | 魔数 `FST0` | 用于快速识别文件类型 |
| version | 1 | 版本号 | 建议从 `1` 开始 |
| iv_len | 1 | IV 长度 | 建议固定 `12`（GCM 推荐长度） |
| iv | iv_len | GCM IV | 每次加密必须随机生成，不可复用 |
| aad_len | 2 | AAD 长度（BE） | 可为 0 |
| aad | aad_len | 绑定上下文的 AAD | 例如 `userIdHash|modelVer|schemaVer` |
| ct_len | 4 | 密文长度（BE） | `ciphertext || tag` 的总长度 |
| ciphertext_and_tag | ct_len | 密文+Tag | Tag 建议 128bit |

**AAD 建议（可选但推荐）**：把“能在业务上绑定该模板”的上下文字段写入 AAD（不加密但参与完整性校验），例如：`user_id_hash`、`embedding_dim`、`model_version`、`feature_schema_version`。这样即便密文被复制到其他用户条目下，也会因 AAD 不匹配而解密失败（等价于“额外绑定”）。

##### 6.5.1.1 Kotlin 关键代码模板（Keystore Key + AES-GCM 加解密 + 原子写入）
```kotlin
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.security.GeneralSecurityException
import java.security.KeyStore
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

sealed class FeatureCryptoError(message: String, cause: Throwable? = null) : Exception(message, cause) {
    class KeyPermanentlyInvalidated(cause: Throwable) : FeatureCryptoError("Keystore 密钥已失效/不可恢复", cause)
    class CorruptedOrTampered(cause: Throwable) : FeatureCryptoError("密文损坏或被篡改（GCM 校验失败）", cause)
    class IoFailure(cause: Throwable) : FeatureCryptoError("文件读写失败", cause)
    class CryptoFailure(cause: Throwable) : FeatureCryptoError("加解密失败", cause)
    class BadFormat : FeatureCryptoError("文件格式不合法或版本不兼容")
}

object FeatureCrypto {
    private const val ANDROID_KEYSTORE = "AndroidKeyStore"
    private const val KEY_ALIAS = "rk3288_feature_aes_gcm_v1"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"
    private const val MAGIC = "FST0"
    private const val VERSION: Byte = 1
    private const val IV_LEN: Int = 12
    private const val TAG_LEN_BITS: Int = 128

    private fun getOrCreateKey(): SecretKey {
        val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        val existing = (ks.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)?.secretKey
        if (existing != null) return existing

        val keyGenerator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE)
        val spec = KeyGenParameterSpec.Builder(
            KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
        )
            .setKeySize(256)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setRandomizedEncryptionRequired(true)
            .build()
        keyGenerator.init(spec)
        return keyGenerator.generateKey()
    }

    fun encrypt(plaintext: ByteArray, aad: ByteArray?): ByteArray {
        try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey())
            if (aad != null && aad.isNotEmpty()) cipher.updateAAD(aad)
            val iv = cipher.iv
            require(iv.size == IV_LEN) { "GCM IV 长度异常: ${iv.size}" }
            val ct = cipher.doFinal(plaintext)
            return encodeEnvelope(iv = iv, aad = aad ?: byteArrayOf(), ciphertextAndTag = ct)
        } catch (e: GeneralSecurityException) {
            throw FeatureCryptoError.CryptoFailure(e)
        } catch (e: IllegalArgumentException) {
            throw FeatureCryptoError.CryptoFailure(e)
        }
    }

    fun decrypt(envelope: ByteArray, aadExpected: ByteArray?): ByteArray {
        val parsed = try {
            decodeEnvelope(envelope)
        } catch (e: FeatureCryptoError.BadFormat) {
            throw e
        } catch (e: Exception) {
            throw FeatureCryptoError.BadFormat()
        }

        val aad = parsed.aad
        if (aadExpected != null && !aad.contentEquals(aadExpected)) {
            throw FeatureCryptoError.CorruptedOrTampered(IllegalStateException("AAD 不匹配"))
        }

        try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            val spec = GCMParameterSpec(TAG_LEN_BITS, parsed.iv)
            cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(), spec)
            if (aad.isNotEmpty()) cipher.updateAAD(aad)
            return cipher.doFinal(parsed.ciphertextAndTag)
        } catch (e: AEADBadTagException) {
            throw FeatureCryptoError.CorruptedOrTampered(e)
        } catch (e: GeneralSecurityException) {
            val msg = (e.message ?: "")
            val isPermanentlyInvalidated =
                msg.contains("Key", ignoreCase = true) && msg.contains("invalid", ignoreCase = true)
            if (isPermanentlyInvalidated) throw FeatureCryptoError.KeyPermanentlyInvalidated(e)
            throw FeatureCryptoError.CryptoFailure(e)
        }
    }

    fun writeEncryptedAtomically(targetFile: File, plaintext: ByteArray, aad: ByteArray?) {
        val tmp = File(targetFile.parentFile, "${targetFile.name}.tmp")
        try {
            val blob = encrypt(plaintext, aad)
            FileOutputStream(tmp).use { out ->
                out.write(blob)
                out.fd.sync()
            }
            if (targetFile.exists() && !targetFile.delete()) {
                throw FeatureCryptoError.IoFailure(IllegalStateException("无法删除旧文件: ${targetFile.absolutePath}"))
            }
            if (!tmp.renameTo(targetFile)) {
                throw FeatureCryptoError.IoFailure(IllegalStateException("无法原子替换文件: ${targetFile.absolutePath}"))
            }
        } catch (e: FeatureCryptoError) {
            tmp.delete()
            throw e
        } catch (e: Exception) {
            tmp.delete()
            throw FeatureCryptoError.IoFailure(e)
        }
    }

    fun readDecrypted(targetFile: File, aadExpected: ByteArray?): ByteArray {
        try {
            val bytes = FileInputStream(targetFile).use { it.readBytes() }
            return decrypt(bytes, aadExpected)
        } catch (e: FeatureCryptoError) {
            throw e
        } catch (e: Exception) {
            throw FeatureCryptoError.IoFailure(e)
        }
    }

    private data class Envelope(val iv: ByteArray, val aad: ByteArray, val ciphertextAndTag: ByteArray)

    private fun encodeEnvelope(iv: ByteArray, aad: ByteArray, ciphertextAndTag: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(MAGIC.toByteArray(Charsets.US_ASCII))
        out.write(byteArrayOf(VERSION))
        out.write(byteArrayOf(iv.size.toByte()))
        out.write(iv)
        out.write(byteArrayOf(((aad.size ushr 8) and 0xFF).toByte(), (aad.size and 0xFF).toByte()))
        out.write(aad)
        out.write(
            byteArrayOf(
                ((ciphertextAndTag.size ushr 24) and 0xFF).toByte(),
                ((ciphertextAndTag.size ushr 16) and 0xFF).toByte(),
                ((ciphertextAndTag.size ushr 8) and 0xFF).toByte(),
                (ciphertextAndTag.size and 0xFF).toByte()
            )
        )
        out.write(ciphertextAndTag)
        return out.toByteArray()
    }

    private fun decodeEnvelope(input: ByteArray): Envelope {
        fun u8(b: Byte): Int = b.toInt() and 0xFF
        fun requireAt(cond: Boolean) { if (!cond) throw FeatureCryptoError.BadFormat() }

        var i = 0
        requireAt(input.size >= 4 + 1 + 1 + 12 + 2 + 4)
        val magic = String(input.copyOfRange(0, 4), Charsets.US_ASCII)
        requireAt(magic == MAGIC)
        i += 4
        val ver = input[i++]
        requireAt(ver == VERSION)
        val ivLen = u8(input[i++])
        requireAt(ivLen == IV_LEN)
        requireAt(i + ivLen <= input.size)
        val iv = input.copyOfRange(i, i + ivLen)
        i += ivLen
        requireAt(i + 2 <= input.size)
        val aadLen = (u8(input[i]) shl 8) or u8(input[i + 1])
        i += 2
        requireAt(i + aadLen <= input.size)
        val aad = input.copyOfRange(i, i + aadLen)
        i += aadLen
        requireAt(i + 4 <= input.size)
        val ctLen =
            (u8(input[i]) shl 24) or (u8(input[i + 1]) shl 16) or (u8(input[i + 2]) shl 8) or u8(input[i + 3])
        i += 4
        requireAt(ctLen > 0)
        requireAt(i + ctLen == input.size)
        val ct = input.copyOfRange(i, i + ctLen)
        return Envelope(iv = iv, aad = aad, ciphertextAndTag = ct)
    }
}
```

##### 6.5.1.1（补充）Java 示例（KeyStore + KeyGenerator + GCMParameterSpec）
```java
import java.nio.ByteBuffer;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public final class FeatureCryptoJava {
    private static final String ANDROID_KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS = "rk3288_feature_aes_gcm_v1";
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";
    private static final byte[] MAGIC = new byte[]{ 'F', 'S', 'T', '0' };
    private static final byte VERSION = 1;
    private static final int IV_LEN = 12;
    private static final int TAG_LEN_BITS = 128;

    private FeatureCryptoJava() {}

    private static SecretKey getOrCreateKey() throws Exception {
        KeyStore ks = KeyStore.getInstance(ANDROID_KEYSTORE);
        ks.load(null);
        KeyStore.Entry entry = ks.getEntry(KEY_ALIAS, null);
        if (entry instanceof KeyStore.SecretKeyEntry) {
            return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        }

        KeyGenerator kg = KeyGenerator.getInstance("AES", ANDROID_KEYSTORE);
        android.security.keystore.KeyGenParameterSpec spec =
                new android.security.keystore.KeyGenParameterSpec.Builder(
                        KEY_ALIAS,
                        android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                                | android.security.keystore.KeyProperties.PURPOSE_DECRYPT
                )
                        .setKeySize(256)
                        .setBlockModes(android.security.keystore.KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(android.security.keystore.KeyProperties.ENCRYPTION_PADDING_NONE)
                        .setRandomizedEncryptionRequired(true)
                        .build();
        kg.init(spec);
        return kg.generateKey();
    }

    public static byte[] encryptToEnvelope(byte[] plaintext, byte[] aad) throws Exception {
        Cipher cipher = Cipher.getInstance(TRANSFORMATION);
        SecretKey key = getOrCreateKey();
        cipher.init(Cipher.ENCRYPT_MODE, key);
        if (aad != null && aad.length > 0) cipher.updateAAD(aad);
        byte[] iv = cipher.getIV();
        if (iv == null || iv.length != IV_LEN) throw new IllegalStateException("GCM IV 长度异常");
        byte[] ciphertextAndTag = cipher.doFinal(plaintext);
        return encodeEnvelope(iv, aad == null ? new byte[0] : aad, ciphertextAndTag);
    }

    public static byte[] decryptFromEnvelope(byte[] envelope, byte[] aadExpected) throws Exception {
        ParsedEnvelope p = decodeEnvelope(envelope);
        if (aadExpected != null && !java.util.Arrays.equals(p.aad, aadExpected)) {
            throw new IllegalStateException("AAD 不匹配");
        }

        Cipher cipher = Cipher.getInstance(TRANSFORMATION);
        SecretKey key = getOrCreateKey();
        GCMParameterSpec spec = new GCMParameterSpec(TAG_LEN_BITS, p.iv);
        cipher.init(Cipher.DECRYPT_MODE, key, spec);
        if (p.aad.length > 0) cipher.updateAAD(p.aad);
        return cipher.doFinal(p.ciphertextAndTag);
    }

    private static byte[] encodeEnvelope(byte[] iv, byte[] aad, byte[] ciphertextAndTag) {
        int size = 4 + 1 + 1 + iv.length + 2 + aad.length + 4 + ciphertextAndTag.length;
        ByteBuffer buf = ByteBuffer.allocate(size);
        buf.put(MAGIC);
        buf.put(VERSION);
        buf.put((byte) iv.length);
        buf.put(iv);
        buf.putShort((short) aad.length);
        buf.put(aad);
        buf.putInt(ciphertextAndTag.length);
        buf.put(ciphertextAndTag);
        return buf.array();
    }

    private static final class ParsedEnvelope {
        final byte[] iv;
        final byte[] aad;
        final byte[] ciphertextAndTag;

        ParsedEnvelope(byte[] iv, byte[] aad, byte[] ciphertextAndTag) {
            this.iv = iv;
            this.aad = aad;
            this.ciphertextAndTag = ciphertextAndTag;
        }
    }

    private static ParsedEnvelope decodeEnvelope(byte[] input) {
        if (input == null || input.length < (4 + 1 + 1 + IV_LEN + 2 + 4)) {
            throw new IllegalArgumentException("文件格式不合法");
        }
        ByteBuffer buf = ByteBuffer.wrap(input);
        byte[] magic = new byte[4];
        buf.get(magic);
        if (!java.util.Arrays.equals(magic, MAGIC)) throw new IllegalArgumentException("magic 不匹配");
        byte ver = buf.get();
        if (ver != VERSION) throw new IllegalArgumentException("version 不兼容");
        int ivLen = buf.get() & 0xFF;
        if (ivLen != IV_LEN) throw new IllegalArgumentException("IV 长度不合法");
        byte[] iv = new byte[ivLen];
        buf.get(iv);
        int aadLen = buf.getShort() & 0xFFFF;
        if (aadLen < 0 || aadLen > buf.remaining()) throw new IllegalArgumentException("AAD 长度不合法");
        byte[] aad = new byte[aadLen];
        buf.get(aad);
        int ctLen = buf.getInt();
        if (ctLen <= 0 || ctLen != buf.remaining()) throw new IllegalArgumentException("密文长度不合法");
        byte[] ct = new byte[ctLen];
        buf.get(ct);
        return new ParsedEnvelope(iv, aad, ct);
    }
}
```

##### 6.5.1.2 异常处理与降级策略（必须固定口径）
<a id="tbl-6-10"></a>
#### 表 6-10 特征加密存储异常映射与处理策略（建议直接照表实现）
| 场景 | 典型异常/表现 | 风险含义 | 推荐处理（门禁业务口径） |
| :--- | :--- | :--- | :--- |
| 密文被篡改/损坏 | `AEADBadTagException` 或 AAD 不匹配 | 数据不可信 | 视为该条模板损坏：删除该用户模板文件并触发重新注册/重新采集 |
| Keystore 密钥不可用 | `UnrecoverableKeyException`/`KeyStoreException`/设备策略导致的失效 | 历史数据无法解密 | 删除密钥别名与所有模板文件；引导重新注册；写入 `ErrorLog/` 便于审计 |
| 读写失败 | `IOException`/rename 失败 | 数据可能丢失或不完整 | 保留 tmp 文件（或清理并重试）；必要时降级为“只读模式”并提示空间/权限 |
| 版本不兼容 | magic/version 不匹配 | 协议漂移 | 拒绝加载并提示升级/迁移；禁止 silent fallback（避免误用错误口径） |

##### 6.5.1.3 关键约束（必须写进工程检查清单）
- 禁止复用 IV：同一 key 下 IV 重复会破坏 GCM 安全性；必须每次随机生成。
- 禁止把 user_id 明文写入文件名：文件名可用 hash/随机 id，并把映射放在受控 DB（可同样加密）中。
- Key Alias 与协议版本绑定：`feature_aes_gcm_v1` 与 envelope `version=1` 成对出现，便于升级到 v2（例如增加压缩/分片/密钥轮换）。

#### 6.5.2 性能优化要点（端侧）
- 摄像头侧：CameraX Preview + ImageAnalysis 背压策略（KEEP_ONLY_LATEST），避免分析阻塞。
- 预处理侧：复用中间 Buffer，减少 `Bitmap` 与大对象频繁分配。
- 推理侧：固定线程池，避免每帧创建线程；热身推理（warm-up）后再开始计时。
- 检索侧：N≤10000 时优先采用向量化加速与批量相似度计算；输出 TopK 需要固定排序策略与稳定阈值。

### 6.6 可复现 Demo 交付：构建/测试/CI 最小模板

#### 6.6.1 Demo APK 构建脚本模板
本节交付目标是：给出一个“可被 CI 直接跑起来”的 Demo 工程模板，并把 **Gradle/NDK/测试框架/门禁规则** 固定为可审计资产，避免版本漂移导致“昨天能构建、今天不能构建”。

**模板口径（示例，不强制等同于本仓库当前版本）**：
- Gradle Wrapper：7.5.1
- Android Gradle Plugin（AGP）：7.4.2（与 Gradle 7.5.x 兼容）
- JDK：11（AGP 7.4 推荐口径；若改 17，需同步验证）
- NDK：25.2.9519653（25.x 系列）

Demo 目录建议（可放在 `docs/examples/demo_android/`，或独立仓库）：
```text
demo_android/
├── gradle/wrapper/gradle-wrapper.properties
├── settings.gradle
├── build.gradle
├── gradle.properties
└── app/
    ├── build.gradle
    ├── src/main/AndroidManifest.xml
    ├── src/main/java/.../MainActivity.kt
    ├── src/main/cpp/CMakeLists.txt
    ├── src/main/cpp/native-lib.cpp
    ├── src/test/java/.../ThresholdPolicyTest.kt
    ├── src/test/java/.../RobolectricSmokeTest.kt
    └── src/androidTest/java/.../CameraUiTest.kt
```

Gradle Wrapper 固定（`gradle/wrapper/gradle-wrapper.properties`）：
```properties
distributionUrl=https\://services.gradle.org/distributions/gradle-7.5.1-bin.zip
```

`settings.gradle`（插件与仓库固定，避免镜像漂移）：
```gradle
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "demo_android"
include(":app")
```

根 `build.gradle`（AGP 固定）：
```gradle
plugins {
    id "com.android.application" version "7.4.2" apply false
    id "org.jetbrains.kotlin.android" version "1.8.22" apply false
}
```

`gradle.properties`（稳定性与内存口径）：
```properties
android.useAndroidX=true
org.gradle.jvmargs=-Xmx4g -Dfile.encoding=UTF-8
org.gradle.daemon=false
```

`app/build.gradle`（NDK25 + CMake + 测试依赖最小集）：
```gradle
plugins {
    id "com.android.application"
    id "org.jetbrains.kotlin.android"
}

android {
    namespace "com.example.demo"
    compileSdk 34

    defaultConfig {
        applicationId "com.example.demo"
        minSdk 21
        targetSdk 34
        versionCode 1
        versionName "v0.1beta1"

        ndkVersion "25.2.9519653"
        ndk {
            abiFilters "armeabi-v7a", "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                cppFlags "-std=c++17 -fno-exceptions -fno-rtti"
                arguments "-DANDROID_STL=c++_shared"
            }
        }

        testInstrumentationRunner "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            minifyEnabled false
        }
    }

    externalNativeBuild {
        cmake {
            path "src/main/cpp/CMakeLists.txt"
            version "3.22.1"
        }
    }

    testOptions {
        unitTests {
            includeAndroidResources true
        }
    }
}

dependencies {
    implementation "androidx.core:core-ktx:1.13.1"
    implementation "androidx.appcompat:appcompat:1.7.0"
    implementation "com.google.android.material:material:1.12.0"

    testImplementation "junit:junit:4.13.2"
    testImplementation "org.robolectric:robolectric:4.12.2"
    testImplementation "androidx.test:core:1.6.1"

    androidTestImplementation "androidx.test.ext:junit:1.2.1"
    androidTestImplementation "androidx.test.espresso:espresso-core:3.6.1"

    androidTestImplementation "androidx.camera:camera-core:1.3.4"
    androidTestImplementation "androidx.camera:camera-camera2:1.3.4"
    androidTestImplementation "androidx.camera:camera-lifecycle:1.3.4"
    androidTestImplementation "androidx.camera:camera-testing:1.3.4"
}
```

`src/main/cpp/CMakeLists.txt`（NDK demo 最小模板）：
```cmake
cmake_minimum_required(VERSION 3.22.1)
project(demo_android)

add_library(native-lib SHARED native-lib.cpp)

find_library(log-lib log)
target_link_libraries(native-lib ${log-lib})
```

#### 6.6.2 JUnit + Robolectric（单测）与 Espresso + MockCamera（集测）模板
**JUnit（纯逻辑）**：只测“可复现口径”的逻辑，不依赖 Android 框架，例如：阈值策略、TopK 稳定排序、序列化格式稳定。
```kotlin
import org.junit.Assert.assertEquals
import org.junit.Test

class ThresholdPolicyTest {
    @Test
    fun `cosine threshold should be stable`() {
        val threshold = 0.80
        val score = 0.801
        val decision = if (score >= threshold) "PASS" else "REJECT"
        assertEquals("PASS", decision)
    }
}
```

**Robolectric（轻量 Android 行为）**：验证资源可用、Context 行为、以及“不会意外崩溃”的 smoke test。注意：Robolectric 对 Keystore/硬件相关能力不应当作为真实性验证口径，Keystore 相关必须由真机/模拟器上的 instrumentation 覆盖。
```kotlin
import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertNotNull
import org.junit.Test

class RobolectricSmokeTest {
    @Test
    fun `application context should be available`() {
        val ctx = ApplicationProvider.getApplicationContext<Context>()
        assertNotNull(ctx)
    }
}
```

**Espresso + MockCamera（集测）**：目标是把“UI 流程 + 相机恢复能力”做成可自动化回归。推荐两条路径：
- 路径 A（优先）：对“相机帧输入”做抽象（FrameSource），instrumentation 用 Fake/录制帧回放实现；UI 只验证权限/回流/状态机，不强依赖真摄像头。
- 路径 B（CameraX 测试库）：使用 `androidx.camera:camera-testing` 提供的 Fake/Mock 能力，验证 `bind/unbind`、异常回调与恢复策略不回归。

Espresso 用例骨架（权限回流 + 页面不崩溃口径）：
```kotlin
import androidx.test.ext.junit.rules.ActivityScenarioRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.rule.GrantPermissionRule
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CameraUiTest {
    @get:Rule
    val grant: GrantPermissionRule = GrantPermissionRule.grant(android.Manifest.permission.CAMERA)

    @get:Rule
    val rule = ActivityScenarioRule(MainActivity::class.java)

    @Test
    fun openActivity_shouldNotCrash() {
        rule.scenario.onActivity { }
    }
}
```

#### 6.6.3 GitHub Actions：构建 + 测试 + 门禁（识别率≥97% 且 泄漏≤5MB）模板
本模板把门禁拆成两类：PR 快速门禁（分钟级）与 Nightly 设备门禁（小时级）。PR 门禁不强求跑出最终识别率，而是验证“口径不漂移 + 指标解析链路有效”；Nightly 门禁可接真机 Runner 或自研设备农场。

> 边界说明：
> - 下述内容为“参考模板”，用于说明门禁拆分与指标口径；不代表本仓库已启用 `.github/workflows/android-ci.yml`。
> - 本仓库当前实际启用的 CI 工作流为：`.github/workflows/ci.yml`（仓库卫生 + 最小单测/构建验证）。

<a id="tbl-6-11"></a>
#### 表 6-11 CI 门禁阈值（建议默认值，可按产品定义调整）
| 指标 | 默认阈值 | 产出来源 | 失败处理 |
| :--- | ---: | :--- | :--- |
| Top1 识别率（Accuracy） | ≥ 0.97 | `tests/metrics/accuracy.json` | 阻断合并 |
| 泄漏增长（Leak Delta） | ≤ 5 MB | `tests/metrics/leak.json` | 阻断合并 |

GitHub Actions 工作流（`.github/workflows/android-ci.yml` 模板，含门禁解析）：
```yaml
name: android-ci
on:
  pull_request:
  push:
    branches: [ "main" ]

jobs:
  pr-gate:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: "11"
      - uses: android-actions/setup-android@v3
      - name: Build + Unit Tests
        run: ./gradlew --no-daemon clean assembleDebug testDebugUnitTest lintDebug

      - name: Upload test reports
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: reports
          path: |
            **/build/reports/**
            **/build/test-results/**

      - name: Evaluate gates (accuracy + leak)
        run: python3 scripts/ci/evaluate_gates.py tests/metrics/accuracy.json tests/metrics/leak.json

  pr-instrumentation:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: "11"
      - uses: android-actions/setup-android@v3
      - name: Connected Android Tests (Emulator)
        uses: reactivecircus/android-emulator-runner@v2
        with:
          api-level: 30
          arch: x86_64
          target: google_apis
          disable-animations: true
          emulator-options: -no-window -no-audio -no-boot-anim -gpu swiftshader_indirect
          script: ./gradlew --no-daemon connectedDebugAndroidTest

  nightly-device-gate:
    if: github.event_name == 'push'
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: "11"
      - uses: android-actions/setup-android@v3
      - name: Nightly placeholder
        run: echo "此处建议改为自建 Runner + 真机基准（产出 CSV/JSON 并复用同一 evaluate 脚本）"
```

门禁解析脚本模板（`scripts/ci/evaluate_gates.py`，以 JSON 为准，避免解析 logcat 漂移）：
```python
import json
import sys

ACCURACY_MIN = 0.97
LEAK_MB_MAX = 5.0

def read_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

def main() -> int:
    if len(sys.argv) != 3:
        print("usage: evaluate_gates.py <accuracy.json> <leak.json>")
        return 2
    acc = read_json(sys.argv[1])
    leak = read_json(sys.argv[2])

    accuracy = float(acc["top1_accuracy"])
    leak_mb = float(leak["leak_delta_mb"])

    ok = True
    if accuracy < ACCURACY_MIN:
        ok = False
        print(f"[GATE] accuracy fail: {accuracy:.4f} < {ACCURACY_MIN:.4f}")
    else:
        print(f"[GATE] accuracy ok: {accuracy:.4f} >= {ACCURACY_MIN:.4f}")

    if leak_mb > LEAK_MB_MAX:
        ok = False
        print(f"[GATE] leak fail: {leak_mb:.2f} MB > {LEAK_MB_MAX:.2f} MB")
    else:
        print(f"[GATE] leak ok: {leak_mb:.2f} MB <= {LEAK_MB_MAX:.2f} MB")

    return 0 if ok else 1

if __name__ == "__main__":
    raise SystemExit(main())
```

指标文件格式建议（由测试/基准产出，稳定可审计）：
```json
{ "top1_accuracy": 0.9731, "dataset_id": "test_set01", "build_id": "v0.1beta1" }
```
```json
{ "leak_delta_mb": 3.2, "scenario": "camera_open_close_50x", "build_id": "v0.1beta1" }
```

### 6.7 本仓库已落地的 YOLO+ArcFace 工程骨架（RK3288）

本小节用于把“方案研究”同步为可执行入口，便于后续重构时对齐口径与复现。

#### 6.7.1 关键代码入口（按职责）
- YOLO 人脸检测（统一输出 bbox/score/可选 5 点关键点）：`src/cpp/include/YoloFaceDetector.h`
- ArcFace 特征（固定 512D、float32、L2 归一化、版本化）：`src/cpp/include/ArcFaceEmbedder.h`
- 对齐（优先关键点仿射；回退 bbox 裁剪+resize 到 112×112）：`src/cpp/include/FaceAlign.h`
- 1:N 检索（N≤10000 线性 TopK + 稳定排序）：`src/cpp/include/FaceSearch.h`
- 阈值策略（版本化/回滚/连续 K 次通过触发）：`src/cpp/include/ThresholdPolicy.h`
- 闭环管线（事件 JSON + 审计落盘）：`src/cpp/include/FaceInferencePipeline.h`
- 推理对比基准（ncnn vs OpenCV DNN）：`src/cpp/tools/inference_bench_cli.cpp`
- 模板 schema（特征模板二进制格式与版本）：`src/cpp/include/FaceTemplate.h`
- Android 加密落盘（Keystore + AES-GCM，文件名不落明文 user_id）：`src/java/com/example/rk3288_opencv/FeatureTemplateEncryptedStore.java`

#### 6.7.2 设备侧可复现命令（不提交模型，部署侧提供路径）
1) 推理后端对比基准（输出 CSV/JSON 到 `tests/metrics/`）：
```bash
./inference_bench_cli --backend both \
  --opencv-model <model.onnx> \
  --ncnn-param <model.param> --ncnn-bin <model.bin> \
  --w 320 --h 320 --warmup 10 --iters 100 --out-dir tests/metrics
```

2) YOLO 离线图片检测（输出 JSON 到 stdout，并落盘到 `tests/metrics/` 或 `ErrorLog/`）：
```bash
./rk3288_cli --yolo-face <image.jpg> --backend opencv --model <yolo_face.onnx> --w 320 --h 320 --score 0.40 --nms 0.45
```

3) YOLO+ArcFace 识别闭环（输出事件 JSON + 审计落盘）：
```bash
./rk3288_cli --face-infer <image.jpg> \
  --yolo-backend opencv --yolo-model <yolo_face.onnx> --yolo-w 320 --yolo-h 320 \
  --arc-backend opencv --arc-model <arcface.onnx> --arc-w 112 --arc-h 112 \
  --gallery-dir <gallery_dir> --topk 5 --threshold 0.35 --consecutive 1
```

4) 性能基线采集（自动产出 raw/summary CSV + Markdown 报告）：
- 报告模板：`tests/reports/face_baseline/REPORT_TEMPLATE.md`
```bash
./rk3288_cli --face-baseline <imagePath|dir> \
  --warmup 5 --repeat 50 --detect-stride 1 --include-load 0 \
  --yolo-backend opencv --yolo-model <yolo_face.onnx> --yolo-w 320 --yolo-h 320 \
  --arc-backend opencv --arc-model <arcface.onnx> --arc-w 112 --arc-h 112 \
  --gallery-dir <gallery_dir> --topk 5 --face-select score_area \
  --out-dir tests/reports/face_baseline --out-prefix face_baseline
```

### 6.8 分支策略 / 代码规范 / 发布流程（用于文档同步审计）

#### 6.8.1 分支策略（Branch Strategy）
- 主分支：`master`（默认稳定分支，所有合并进入该分支后再做版本化与交付）
- 功能分支：`feature/<topic>`（开发完通过 PR 合并回 `master`）
- 修复分支：`hotfix/<issue>`（紧急问题修复，合并回 `master`）
- 版本标签：`vX.Y.Z`（与 Changelog/构建产物口径一致）

#### 6.8.2 代码规范（Code Style）
- 语言与编码：统一 UTF-8；对外输出（日志/报告/文档）默认使用 ZH_CN。
- 安全：禁止输出密钥/令牌/隐私信息；日志粘贴前必须打码。
- 质量门禁（建议）：至少通过基础构建、核心单测、文档同步审计（`scripts/docs-sync-audit.js`）。

#### 6.8.3 发布流程（Release Process）
1) 运行 `scripts/docs-sync-audit.js`，确保 high 缺陷为 0；若存在 BSP/defconfig/内核配置占位，则先补齐输入再发布。
2) 生成版本号并更新本文件的 Changelog（记录 build_id、模型版本号、阈值版本号等关键口径）。
3) 产出构建物（APK/Native 可执行程序），并附带性能基线报告（`tests/reports/face_baseline/`）。

---

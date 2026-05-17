# 1. 问题

本问题聚焦于 C++ 模块的人脸推理流水线函数 `runFaceInferOnce`：函数体过长、职责过多，且 JSON 序列化路径混用（自定义 `JsonWriter` 与手写 `ostringstream`）。流程控制、错误处理与序列化紧密耦合，导致可读性、可测试性、可扩展性欠佳。

## 1.1. **巨型函数与职责耦合**
- 位置：`src/cpp/src/FaceInferencePipeline.cpp` 第 284–763 行。
- 单个函数串联了全部阶段：图像加载、YOLO 检测、候选选择、对齐、ArcFace 嵌入、图库加载、检索、阈值策略决策、JSON 组装与异常兜底。
- 表现为大量临时状态（如 `stage`, `msg`, 计时指标等）跨越整个函数生命周期，任何新需求都需要在此集中修改，极易引入回归风险。

代表性代码（阶段间状态、控制与数据混杂）：
```cpp
FaceInferOutcome runFaceInferOnce(const FaceInferRequest& req) {
    FaceInferOutcome out; const long long tsMs = nowEpochMillis();
    try {
        const auto t0 = std::chrono::steady_clock::now();
        cv::Mat img = cv::imread(req.imagePath, cv::IMREAD_COLOR);
        // ... yolo 加载与检测、主脸选择、对齐、arc 初始化与嵌入、图库加载、搜索、阈值决策 ...
        std::string stage, msg; long long msLoad, msDetect, msAlign, msEmbed, msSearch, msTotal;
        // ... 根据各阶段结果决定 stage/msg 并拼装 JSON ...
    } catch (...) { /* 兜底异常 JSON */ }
}
```

## 1.2. **JSON 序列化混用与分散**
- 在错误路径使用自定义 `JsonWriter`，在正常路径与异常兜底使用手写 `ostringstream` + `jsonEscape`，存在两套并行的字段定义与转义逻辑。
- 该混用易造成：字段不一致、顺序不稳定、转义遗漏、重复维护；也使得序列化代码分散在各处，难以覆盖测试。

代表性代码（错误路径：JsonWriter）：
```cpp
j.beginObject(); j.key("ok"); j.boolean(false); j.key("errorCode"); j.number(out.errorCode);
// ... 多层对象写入 ...
out.json = j.str();
```
代表性代码（正常路径：ostringstream）：
```cpp
std::ostringstream jout;
jout << "{\"ok\":" << (out.ok ? "true" : "false") << ",";
// ... 手工拼字段与转义 ...
out.json = jout.str();
```

## 1.3. **错误处理与主流程交织**
- 通过在主流程中不断检查 `stage.empty()` 来穿插错误分支，主流程与错误处理高度耦合。
- `catch` 块直接手写 JSON 字符串，进一步增加分散度与不一致风险。

## 1.4. **扩展难度与回归风险**
- 新增 YOLO/ArcFace 后端、对齐策略、阈值策略版本或指标时，改动无法局部化，易触及该巨型函数多个位置。
- 任何序列化字段调整都需在两套路径中同步维护，扩大失败面。

# 2. 收益

核心收益：在保持外部行为与 JSON 输出不变的前提下，拆分阶段并统一序列化路径，显著降低复杂度，提升可测试性与稳定性。

## 2.1. **降低复杂度**
- 预计将 `runFaceInferOnce` 的圈复杂度从当前的 **30+** 分支点降至约 **10**（主函数仅编排各阶段与汇总），后续新增能力可在独立单元中实现。

## 2.2. **提升可测试性**
- 每个阶段（加载/检测/对齐/嵌入/检索/决策/序列化）可独立编写单元测试与基准测试，异常与边界更易覆盖。

## 2.3. **增强可维护性与扩展性**
- 新增后端或策略时，仅影响对应阶段模块；主流程无须重写，大幅降低触点与回归成本。

## 2.4. **统一序列化的一致性与安全性**
- 采用单一 `JsonWriter`，统一字段、顺序与转义；避免手写字符串拼接带来的转义与逗号错误。

## 2.5. **错误处理更清晰**
- 通过集中式失败构造函数，错误码与 `stage` 映射一致、可复用、易审计。

# 3. 方案

总体思路：引入“阶段化拆分 + 统一 JSON 构建”的结构。以 `FaceInferContext` 承载各阶段的输入输出与计时指标，主函数专注于编排；所有 JSON 输出经单一路径构建，确保行为等价。

## **3.1. 流水线结构示意**
```mermaid
flowchart LR
    A[请求] --> B[加载图像]
    B --> C[人脸检测]
    C --> D[主脸选择]
    D --> E[人脸对齐]
    E --> F[ArcFace嵌入]
    F --> G[图库加载]
    G --> H[TopK检索]
    H --> I[阈值决策]
    I --> J[统一JSON构建]
```

## 3.2. **引入阶段化函数：解决“巨型函数与职责耦合”**
- 新增 `FaceInferContext` 承载中间结果与计时指标。
- 将每个阶段拆分为“小而清晰”的函数，返回显式成功/失败与错误信息。
- 主函数仅负责编排与早返回，不再混杂具体实现细节。

## 3.3. **统一 JsonWriter：解决“JSON 序列化混用与分散”**
- 引入 `OutcomeJsonBuilder`（仅使用 `JsonWriter`）集中维护字段、顺序与转义；提供 `makeSuccessOutcome` 与 `makeFailureOutcome` 两个入口。

## 3.4. **规范错误处理：解决“错误处理与主流程交织”**
- 通过集中式失败构建器与 `errorCodeForStage(stage)` 映射，任何阶段失败立即早返回统一 JSON，主流程不再充斥 `stage.empty()` 判定。

# 4. 回归范围
- 正常识别/无人脸/假检测/假嵌入路径 JSON 字段一致性
- 图像加载失败/YOLO 加载失败/ArcFace 初始化或嵌入失败/图库加载失败/异常兜底
- JSON 字段集、顺序与转义正确性（含中文/特殊字符）

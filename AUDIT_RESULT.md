# 重构洞察审计报告

## 审计范围

依据 `rk3288_opencv` 主仓库代码（worktree: `serene-chaplygin-ea8365`），逐一验证《重构洞察.txt》的所有声明。

---

## 总体评级

**报告健康度: 7/10** — 宏观诊断准确，具体数据存在偏差（夸大+遗漏）。

| 维度 | 评分 | 说明 |
|------|------|------|
| 架构分析 | 8/10 | 重大问题识别正确，Engine 上帝对象、RAII 缺失、INI双源等 | 
| 代码细节准确性 | 5/10 | 多处数字偏差（68字段→51, 9全局→10, 977行→983）, perfHistory OOM 判定错误 |
| 建议质量 | 8/10 | 重构方向合理，RAII封装、事务拷贝、INI统一化等建议扎实 |
| 遗漏 | 4/10 | 漏掉 `postStart` 未声明编译错误（编译期阻断） |

---

## 详细逐条审计

### 致命技术债务

#### 1. Engine 上帝对象 + 全局裸指针
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| Engine.cpp 1286行 | 1294行 | ◆ 可接受偏差 (0.6%) |
| processFrame ~300行 | 319行 (L739-1057) | ◆ 可接受 |
| native-lib.cpp 9个文件作用域静态全局 | **10**个 (L22-31) | ⚠ 少1个 (g_cancelInit) |
| processFrame 操作 faceTracks/perfHistory/renderFrame | ✅ 确认 | 正确 |
| renderMutex 存在 | ✅ Engine.h:151 | 正确 |
| getRenderFrame 行为 | ✅ L1064-1071 | 正确 |
| RAII wrapper 缺失 | ✅ 确认缺失 | 正确（`WindowReleaser` 只负责 `release` 不是 `lock`） |
| frame.copyTo(renderFrame) 性能注释 | ✅ L776-778 | 代码自带说明 |

#### 2. 异常安全缺失与 RAII 违规
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| processFrame 无 try-catch | ✅ 确认 | 零个 try-catch |
| cv::Mat frame = inputFrame | ✅ L755 (报告写 L731, 实际 L755) | 基本正确 |
| ANativeWindow_lock 裸调用 | ✅ L741, L753, L764 | 正确 |
| AndroidBitmap_lockPixels 裸调用 | ✅ L643 | 正确 |

#### 3. FaceInferRequest 68字段贫血模型
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| 68个字段 | **51个**成员变量 | ❌ **夸大33%** (FaceInferencePipeline.h:7-68) |
| OpenCVBackend/NcnnBackend 类 | 不存在, 使用 `std::string` 运行时选择 | ❌ 建议的类不存在 |
| `std::variant<OpenCVBackend, NcnnBackend>` | 项目中 `std::variant` 完全未使用 | ❌ 建议超前 |
| 3个职责结构体方案 | — | 合理方向, 但数字偏差影响可信度 |

**校正**: FaceInferRequest 实际字段分布: 1个imagePath + 19个yolo前缀 + 17个arc前缀 + 5个threshold/gallery + 2个int8 + 2个fake/debug + 若干杂项 = 51字段.

#### 4. CV::Mat 悬挂引用与内存安全
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| renderMutex 保护 copyTo | ✅ L1046-1051 | 正确 |
| getRenderFrame 加锁 copyTo | ✅ L1065-1068 | 正确 |
| putText/rectangle 在 processFrame 中 | ✅ L784, L807, L822, L984, L999 | 正确 |
| 无 double buffering | ✅ 单帧 + mutex | 正确 |

---

### 架构设计缺陷

#### 1. CMakeLists.txt 977行
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| 977行 | **983行** | ⚠ 偏差6行 |
| OpenCV add_subdirectory 源码构建 | ✅ L225 | 正确 |
| 全局 include_directories 17个模块 | ✅ L229-247 (实际15个) | ◆ 基本正确 |
| 15个 target | ✅ 确认 | 正确 |
| BUILD_LIST 裁剪 | ✅ CMakePresets.json + 构建脚本 | 正确, 但不在 CMakeLists.txt 本身 |

#### 2. Java INI + SharedPreferences 双源
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| persistInferenceThrottleIni | ✅ MainActivity.java:1050 | 正确 |
| readInferenceIniIfExists | ✅ MainActivity.java:1197 | 正确 |
| SharedPreferences 使用 | ✅ 5个Java文件 | 正确 |

#### 3. 自定义测试框架 + 废弃报告
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| docs-sync-audit 44个文件 | **47个** (23对 json+md + link-cache.json) | ⚠ 少3个 |
| 自定义 TestCase 结构 | ✅ face_infer_unit_tests_main.cpp:7-10 | 正确 |
| ASSERT_EQ 宏不存在 | ✅ 确认 | 正确, 建议合理 |
| test_resource_cleanup.cpp | ✅ tests/cpp/ 47行 | 正确 |

#### 4. Web SPA 类型污染
| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| ServerSettingsDoc 存在 | ✅ types.ts:21 | 正确 |
| "DirectX" 字段名 | ⚠ 字段名 GPU无关 | **夸大**: 参数名是通用渲染参数, 非 DirectX 专有 |
| Cypress continue-on-error ci.yml:130-136 | ✅ ci.yml:130-137 | **正确, 精确匹配行号** |
| preview.mjpeg 端点 | ✅ HttpFacesServer.cpp:626,1003 | 正确 |

---

### 未来适应性评估

| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| perfHistory "无上限增长可触发OOM" | **30条后清空** (L621-678, 含CSV导出) | ❌ **错误判定** — 有明确上限和清空机制 |
| handleAbnormalEvent 每分钟30张JPEG | 有指数退避节流 (最高10s冷却, L1078-1168) | ⚠ 部分正确, 有节流逻辑被忽略 |
| RKLOG_ENTER 宏 | ✅ NativeLog.h:13, Engine.cpp 8处 | 正确 |
| config/windows_camera_face_recognition.ini | ✅ 存在 | 正确 |
| Engine 硬编码单 VideoManager | ✅ Engine.h:128 | 正确 |
| externalInput 通道 | ✅ Engine.h:172 FrameInputChannel | 报告未提及此已存在的解耦入口 |
| 缺失抽象 (MemoryArena, ConfigTree, PipelineStage, TraceSpan) | ✅ 确实不存在 | 合理建议 |

---

### 技术栈演进建议

| 报告声明 | 实际值 | 判定 |
|----------|--------|------|
| C++17 → C++20 | 当前 C++17 (CMakeLists.txt:10) | 合理但需验证 NDK r27+ 兼容性 |
| std::span 未使用 | ✅ 确认 | 正确, 合理建议 |
| std::expected 未使用 | ✅ 确认 | C++23, 只能用 tl::expected 替代 |
| std::variant 未使用 | ✅ 确认 | C++17 可用但未用 |
| constexpr 优化建议 | — | 合理 |
| CMake 模块拆分 | 不存在 cmake/ 目录 | 合理建议 |
| -fno-exceptions / -fno-rtti | 仅在 DEVELOP.md L1951 作为示例 | ✅ 正确标识未启用 |
| -fvisibility=hidden | 未使用 | ✅ 正确标识 |
| -Werror 完整 | 仅 -Werror=format-security (L122) | ⚠ 建议全开需注意 OpenCV 源码 warning |

---

### 紧急行动项

| 报告声明 | 判定 | 备注 |
|----------|------|------|
| RAII wrapper: ScopedWindowLock / ScopedBitmapLock | ✅ 合理紧急 | ANativeWindow_lock(L741) 和 AndroidBitmap_lockPixels(L643) 无保护 |
| 事务拷贝 faceTracks/perfHistory | ◆ 合理 | 但 perfHistory 已有 clearing 机制 |
| 删除 INI 代码 | ✅ 合理 | 120+行可安全移除 |

---

## 审计发现的新问题（报告未提及）

### **严重: `postStart` 未声明变量 — Engine.cpp:1043**

```cpp
stats.postMs = static_cast<double>(nowMs() - postStart);
```

`postStart` 在 `processFrame()` 中**未被声明**（函数局部/类成员/文件作用域均无定义）。所有正常 C++ 编译器应报**编译错误**。

对比其他计时变量均有合法声明: `preStart` (L749), `inferStart` (L769), `renderStart` (L1044)。`postStart` 是唯一缺失的。

**影响**: 此问题阻断编译。如项目确实可编译，需排查是否有未被检测到的宏/条件编译绕过，否则这是需要立即修复的阻断性 bug。

### **次要: project uses C++17 not C++20**

报告中 "C++17 → C++20 子集迁移（立即启动）" 的建议误以为项目已在 C++20 上。

### 次要: `externalInput` 通道已存在

报告说 `Engine` 硬编码单 `VideoManager`，但 Engine 已存在 `externalInput` (`FrameInputChannel`) 作为替代帧输入通道，支持 `LatestOnly`/`BoundedQueue` 两种背压模式。

---

## 正确性统计

| 类别 | 正确 | 有偏差 | 错误 |
|------|------|--------|------|
| 致命技术债务 (4项) | 12/16 | 3/16 | 1/16 (68字段) |
| 架构缺陷 (4项) | 13/14 | 1/14 | 0/14 |
| 未来适应性 (6项) | 7/10 | 2/10 | 1/10 (perfHistory) |
| 技术栈建议 (10项) | 9/10 | 1/10 | 0/10 |
| 紧急行动项 (3项) | 3/3 | — | — |
| **合计** | **44/53 (83%)** | **7/53 (13%)** | **2/53 (4%)** |

---

## 结论

**报告整体可信但需修正以下具体错误**:

1. **❌ `FaceInferRequest` 字段数**: 68 → **51** (夸大 33%)
2. **❌ `perfHistory` OOM 警告**: 实际有**30条上限+定期清空+CSV导出**, 不会 OOM
3. **⚠ `native-lib.cpp` 全局变量**: 9 → **10** (漏计 `g_cancelInit`)
4. **⚠ 多处行数/文件数微偏** (977→983, 44→47, 17→15)
5. **⚠ "DirectX 字段" 夸大**: 实际 GPU 无关通用显示参数
6. **❌ 遗漏: `postStart` 未声明 → 编译错误** (Engine.cpp:1043)
7. **⚠ 项目实际使用 C++17**, 不是报告中假设的 C++20
8. **⚠ `externalInput` 通道已存在**, 不是报告说的"硬编码单通道无解耦"

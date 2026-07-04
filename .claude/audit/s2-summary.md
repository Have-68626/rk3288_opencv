# S2 模块逐文件审查 — 汇总索引

**日期**: 2026-07-04 | **状态**: ✅ 全部完成

---

## 产出清单

| # | 模块 | 报告 | 文件数 | 评分 | 核心发现 |
|:--|:-----|:----|:------:|:----:|:---------|
| 🔷 P2.1 | C++ 核心引擎 | [p2.1-cpp-engine.md](p2.1-cpp-engine.md) | ~50 | **2.8/5 中** | 可测试性1.9最低；FaceInferStages全局静态缓存；Engine无DI |
| 🪟 P2.2 | Windows 服务 | [p2.2-windows-service.md](p2.2-windows-service.md) | ~30 | **2.65/5 中** | FramePipeline花括号未闭合；WinJsonConfig/HttpFacesServer肥胖 |
| 📱 P2.3 | Android 层 | [p2.3-android-layer.md](p2.3-android-layer.md) | ~49 | **3.1/5 中** | MainActivity 3155行God模式；安全实践好(KeyStore+AES-GCM) |
| 🌐 P2.4 | Web SPA | [p2.4-web-spa.md](p2.4-web-spa.md) | ~20 | **3.7/5 良** | MJPEG流泄漏；API层质量高；组件膨胀 |
| 🔧 P2.5 | 构建/CI | [p2.5-build-ci.md](p2.5-build-ci.md) | ~15 | **2.3/5 中** | CORE_SOURCES重复；编译宏泄漏；CI Windows不触发PR |
| 🧪 P2.6 | 测试覆盖 | [p2.6-test-coverage.md](p2.6-test-coverage.md) | ~10 | **3.0/5 中** | 覆盖率~6%；Engine/BioAuth/MotionDetector零覆盖；自定义框架不足 |
| 📋 P2.7 | 文档合规 | [p2.7-docs-compliance.md](p2.7-docs-compliance.md) | ~25 | **3.0/5 中** | LICENSE文件**缺失**；DEVELOP.md 172KB；两套设计零实现 |

---

## 综合评分雷达

| 维度 | 加权均分 | 最高模块 | 最低模块 |
|:----|:--------:|:---------|:---------|
| 代码可读性 | **3.1** | Web 4.0 | 构建 2.3 |
| 模块化/耦合度 | **2.7** | Web 3.8 | 构建 2.0 |
| 错误处理 | **3.1** | Web 3.9 | 构建 2.0 |
| 性能 | **3.2** | Engine 3.2 | — |
| 可测试性 | **2.2** | Web 4.2 | Windows 2.0 |
| 总体 | **2.9** | Web 3.7 | 构建 2.3 |

**可测试性是全项目最大短板** (2.2/5)

---

## 待确认：是否启动 S3？

S3 将基于 S1+S2 全部产出，执行串行合成与交叉验证：

1. **P3.1** 跨模块接口契约检查 — JNI 签名一致性、HTTP API 契约、配置格式
2. **P3.2** 全局模式匹配 — 重复代码、命名不一致、错误处理模式
3. **P3.3** 评分汇总 + 完整 AUDIT_REPORT.md 生成

**预计耗时**: 20-30 分钟

请回复 **"启动 S3"** 以继续。

# GitHub 安全与质量扫描审计

**扫描工具**: CodeQL + Dependabot
**日期**: 2026-07-05

---

## 1. CodeQL 代码扫描

**状态**: 已启用，30 次分析记录 | **开放告警**: 7 个

| # | 规则 | 文件 | 严重性 | 类型 | 评估 |
|:-:|:-----|:-----|:------:|:-----|:-----|
| 4-7 | Uncontrolled data in path expression | MainActivity.java | 🔴 high | 路径遍历 | 用户提供的路径直接用于文件操作，需输入校验 |
| 3 | Regular expression injection | LogDetailActivity.java | 🟡 high | 注入 | 用户输入构造正则，可被利用做 ReDoS |
| 1-2 | TOCTOU race condition | civetweb.c (third_party) | 🟡 high | 竞态 | 第三方库代码，风险受控 |

### 与质量审计的对应关系

| CodeQL 告警 | 质量审计发现 | 状态 |
|:------------|:------------|:----:|
| MainActivity 路径遍历（#4-7） | 4.4 MainActivity 封装评分 C，字段 public 未封装 | 相关联 — 路径未校验直接使用 |
| LogDetailActivity 正则注入（#3） | 4.5 日志分页评分 B-，过滤逻辑未审计 | 新发现 — 需修复 |

---

## 2. Dependabot 依赖漏洞

**状态**: 19 个开放告警 | **高风险**: 8 个 | **中风险**: 10 个 | **低风险**: 1 个

### 高风险告警（8 个）

| 依赖 | 漏洞 | 影响 | 修复建议 |
|:-----|:-----|:-----|:---------|
| **form-data** | CRLF injection | HTTP 请求走私 | pnpm update form-data |
| **vite** (x2) | `server.fs.deny` bypass | 任意文件读取 | pnpm update vite |
| **vite** | Arbitrary File Read via DevServer WebSocket | 任意文件读取 | pnpm update vite |
| **@babel/core** | Arbitrary File Read via sourceMappingURL | 任意文件读取 | pnpm update @babel/core |
| **@babel/plugin** | Arbitrary code in malicious input | 任意代码执行 | pnpm update @babel |
| **fast-uri** (x2) | Host confusion + path traversal | SSRF/路径穿越 | pnpm update fast-uri |
| **tmp** | Path traversal via unsanitized prefix | 文件写入逃逸 | pnpm update tmp |

### 中等风险告警（10 个）

| 依赖 | 漏洞 | 影响 |
|:-----|:-----|:------|
| **js-yaml** (x2) | DoS via merge key + Prototype pollution | DoS/污染 |
| **launch-editor** | NTLMv2 hash disclosure via UNC | 凭据泄露 |
| **React Router** | Open redirect via protocol-relative URL | 开放重定向 |
| **qs** | TypeError crash via comma-format arrays | DoS |
| **uuid** | Missing buffer bounds check | 缓冲区 |
| **PostCSS** | XSS via unescaped `<style>` | XSS |
| **Vite** | Path traversal in Optimized Deps `.map` | 文件读取 |

---

## 3. Secret Scanning

**状态**: ❌ 未启用

项目包含 AES-256-GCM 加密密钥管理逻辑（WinCrypto.java、FeatureTemplateEncryptedStore.java），建议启用。

---

## 4. 与代码质量审计合并风险矩阵

| 优先级 | 问题 | 来源 | 严重性 | 工作量 |
|:------:|:-----|:------|:------:|:------:|
| **P0** | SettingsPage.tsx Unicode 引号导致编译失败 | 质量审计 | 阻断 | 1 min |
| **P0** | JNI `e.what()` 嵌入 JSON 未转义 | 质量审计 | 高 | 30 min |
| **P0** | LogDetailActivity 正则注入 | CodeQL #3 | 高 | 1 hr |
| **P0** | MainActivity 路径遍历（4处） | CodeQL #4-7 | 高 | 2 hr |
| **P0** | Vite 任意文件读取（3个漏洞） | Dependabot | 高 | 1 hr |
| **P1** | JNI catch JSON 形状不一致 | 质量审计 | 中 | 30 min |
| **P1** | CameraFragment public 字段 | 质量审计 | 中 | 1 hr |
| **P1** | ViewModel MutableLiveData 暴露 | 质量审计 | 中 | 30 min |
| **P1** | evaluateThrottle() 死代码 | 质量审计 | 低 | 30 min |
| **P1** | Dependabot 中等漏洞（10个） | Dependabot | 中 | 2 hr |
| **P2** | MppDecoder.cpp 遗漏分组 | 质量审计 | 低 | 1 hr |
| **P2** | RK_OPENCV_CORE_LIBS 死代码 | 质量审计 | 低 | 30 min |
| **P2** | BioAuth/Motion/Adapter 测试无断言 | 质量审计 | 低 | 2 hr |

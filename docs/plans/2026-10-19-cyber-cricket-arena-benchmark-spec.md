<!-- DOCSYNC_DOC
type=plan
status=proposed
as_of=2026-10-19
-->

# 2026-10-19 基于 rk3288_opencv 构建 AI 赛博斗蛐蛐（SWE-bench 硬核代码修复靶场）技术规范与题目体系 (v2.0 修订版)

本文固化基于本项目（`rk3288_opencv`）构建大语言模型（LLM / Code Agent）硬核代码修复竞技靶场（赛博斗蛐蛐）的可行性评估、六级天梯题目设计及王者级 10 重强耦合缺陷体系。

---

## 1. 靶场定位与前沿推理模型（xhigh）失效背景

针对 **Gemini 4 Argon**、**ChatGPT-6 / GPT-6 Astra**、**Claude Opus 5.5**、**Grok 4.7 (xhigh 推理强度)** 等 2026 前沿推理模型的最新研究显示：
- 模型在极长思维链（20K~60K tokens）下对自我假说产生病态确认偏差（Confirmation Bias）；
- 在多文件、跨语言、深层时序耦合的真实工业场景下，极易在前 2 轮陷入“近端表象压制”与“基线污染”，导致无法收敛。

---

## 2. 六大天梯段位体系 (Bronze to King)

1. **青铜 (Bronze)**：单点局部边界与 NMS 浮点除零（1 轮秒杀，通过率 > 95%）。
2. **白银 (Silver)**：跨模块配置大小写敏感与枚举映射（1~2 轮通过，通过率 ~85%）。
3. **黄金 (Gold)**：`RK_SKIP_OPENCV` 条件编译与 Mock 存根断裂（2 轮左右，通过率 ~65%）。
4. **铂金 (Platinum)**：JNI 异步回调局部引用表溢出（2~3 轮突破，通过率 ~40%）。
5. **钻石 (Diamond)**：多线程 MJPEG 推流与相机掉线的丢失唤醒死锁（3 轮仅 15% 通过）。
6. **王者 (King / 终极灭霸题)**：**十重全链路强耦合死锁体系（2 轮内必死，至少需要 3 轮以上）**。

---

## 3. 王者级题目：十重全链路强耦合缺陷设计 (P1 ~ P10)

针对前沿超强推理模型设计的 10 个深度耦合缺陷网：

1. **P1 (CMake/CRT 堆破坏)**：`CMakeLists.txt` 全局 `/MT` 静态运行时与动态库堆指针混用，引发静默跨堆释放破坏；
2. **P2 (头文件 ABI 错位)**：`FaceRecognition.h` 中 `#pragma pack(push, 4)` 遗漏 `pop`，后续结构体发生 4 字节偏移；
3. **P3 (JNI 引用泄漏)**：`native_face_bridge.cpp` 异步通知未释放 `NewGlobalRef`，锁定 Dalvik GC 堆；
4. **P4 (AB-BA 锁反转)**：`HttpFacesServer.cpp` 的 `sessionMutex_` 与 `logMutex_` 产生死锁；
5. **P5 (套接字背压误关)**：`HttpFacesServer.cpp` 将非阻塞 `WSAEWOULDBLOCK` 误判为断开并关闭句柄；
6. **P6 (丢失唤醒)**：`FramePipeline.cpp` 相机掉线仅 `notify_one()`，引发工作线程永久僵死；
7. **P7 (TOCTOU 符号链接竞争)**：`test_http_faces_server.cpp` 产生 Access Denied，在单测中暴露为 HTTP 504；
8. **P8 (无锁环形缓冲区 ABA 踩踏)**：`RealtimeFacePipeline.cpp` 原子指针无版本号戳，高频覆盖垃圾数据；
9. **P9 (状态机回退无限递归)**：`FramePipeline.cpp` 相机重载广播自身故障，引发栈溢出；
10. **P10 (配置迁移静默归零)**：`WinJsonConfig.cpp` 异常时将重试次数与超时保护强制归零，短路所有系统自愈。

### 模型 3 轮必败机制
- **第 1 轮**：模型必然被表层的 HTTP 504 误导，修改超时常数与路径正则，测试依然超时卡死；
- **第 2 轮**：模型试图通过 `try_lock()` 或 `detach()` 解决死锁，释放了 P2 的内存错位与 P8 的 ABA 踩踏，测试直接报 Access Violation 段错误崩溃；
- **第 3 轮**：面对复杂的 ASan 堆破坏日志，模型无法跨 4 个子系统同时协同修改 8 个文件，轮数耗尽或产生大量单测回归。

---

## 4. 自动化裁判（Referee Harness）与防作弊规则

1. **测试强锁死（Test Immutability）**：每轮评测前强制 `git checkout HEAD -- tests/`，严禁修改断言。
2. **全仓零回归（Zero Regression）**：除目标修复外，项目既有 166 个单测必须 100% 通过。
3. **补丁行数限额（Diff Budget）**：单次代码修改行数不得超过对应源文件的 15%。

<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-20
-->

# RK3288 机器视觉引擎

面向 Android RK3288 设备与 Windows 主机的人脸识别项目，包含 C++ 推理管线、Android 应用、Windows 本地服务和 Web 管理界面。

## 项目简介

| 组成 | 职责 | 主要入口 |
|---|---|---|
| Android 应用 | 摄像头采集、预览、注册与识别、可选 RTMP 推流 | `app/`、`src/java/`、`src/cpp/jni/` |
| C++ 核心 | 检测、对齐、特征提取、检索、追踪与性能记录 | `src/cpp/` |
| Windows 服务 | 摄像头与模型配置、REST API、SSE/MJPEG、静态页面托管 | `src/win/app/win_local_service_main.cpp` |
| Web SPA | 浏览器管理与预览，Vite + React + Ant Design | `web/src/` |

当前开发目标为 **Android 与 Windows**。RK3288 为 Cortex-A17 / ARMv7，设备约束见 [RK3288 约束](docs/RK3288_CONSTRAINTS.md)。Android 构建还包含其他 ABI；这不改变 RK3288 的硬件能力。Linux 编译 CI 保留为非门禁参考，不是当前开发验收目标。

### 能力与验收边界

- 已有实时与单图人脸推理入口；模型、配置、底库和可选依赖须单独准备，仓库克隆后不能直接推定识别可用。
- RK3288 的硬件加速适配未获得完整设备验收；构建成功或请求了加速器，不代表运行时实际启用。
- 已有指定 APK/AAR 的[双机局域网推流记录](docs/audits/2026-10-17-rk3288-runtime-feedback.md)。记录只适用于对应产物和测试范围，不代表新构建、识别精度或长期稳定性通过。
- 活体检测尚未实现；[后台运行及正式隐藏预览](docs/specs/2026-10-17-rtmp-hidden-preview-design.md)仍为待办，不能据现有人脸识别能力宣称防照片或屏幕攻击。
- 架构拆分已有落地，但五条[架构契约](docs/architecture/ARCHITECTURE.md)仍有待办；RAII 的既有闭环仅覆盖指定符号族。

## 文档导航

| 需要做什么 | 从这里开始 |
|---|---|
| 配置依赖、构建和运行测试 | [构建与测试](docs/guides/build-and-test.md) |
| 理解模块、测试框架和开发流程 | [开发指南](DEVELOP.md) |
| 找到全部正式文档 | [文档索引](docs/index.md) |
| 确认模型来源、许可与供应链 | [依赖与模型台账](CREDITS.md)、[供应链账本](docs/security/supply-chain-ledger.md) |
| 准备、转换或导入模型 | [模型存储与消费者](docs/guides/model-storage-and-consumers.md)、[INT8 操作](docs/guides/int8-quantization.md) |
| 配置或部署 Windows 服务 | [配置指南](docs/guides/windows-service/config.md)、[部署与回滚](docs/guides/windows-service/deploy_and_rollback.md) |
| 使用辅助工具、整理本地资产 | [工具入口](scripts/README.md)、[仓库存储布局](docs/guides/repository-storage-layout.md) |
| 为项目执行 AI 协作任务 | [AGENTS.md](AGENTS.md)、[Claude Code 入口](CLAUDE.md) |

## 项目结构

```text
rk3288_opencv/
├── app/                 Android Gradle 装配、资源和可选 AAR
├── src/
│   ├── cpp/             C++ 核心、JNI 与原生工具
│   ├── java/            Android 生产 Java 源码
│   └── win/             Windows 服务、工具和受跟踪的 webroot
├── web/                 Web 源码、单测与 Cypress
├── cmake/               分平台构建模块
├── tests/               C++、Windows、Java 与工具回归及公共夹具
├── scripts/             可复用构建、审计和验收工具
├── models/              模型契约文本；实际权重本地准备
├── deps/                上游模型与依赖输入；多数本地资产不入库
├── docs/                现行指南、设计、审计与历史归档
└── cache/               本地构建、转换中间件、工具和报告，不等于可随意删除
```

新建独立构建放 `cache/builds/`，报告放 `cache/reports/`；`app/build`、`app/.cxx`、`web/node_modules` 保持工具规定布局。已有 CMake 树含绝对路径，不能直接搬移。模型、依赖、AAR 和历史证据按[存储规范](docs/guides/repository-storage-layout.md)分类，忽略规则不构成删除授权。

## 依赖与环境要求

不同子系统分别准备依赖，不要求只开发 Web 的使用者安装完整 Android 工具链。

| 子系统 | 环境要求与约束 |
|---|---|
| C++ / Windows | C++17、CMake、VS2022 x64；完整后端从 OpenCV 4.10.0 源码构建，相关模块需要 opencv_contrib。核心测试可不依赖 OpenCV |
| Android | 仓库 Gradle Wrapper 9.0，CI 使用 Temurin Java 21；Java 源码级别 11；Android SDK/NDK 与 CMake 要求见构建指南及 Gradle 配置 |
| Web | CI 使用 Node.js 22、pnpm 10.33.0；按锁文件冻结安装，Cypress 二进制另行准备 |
| 可选组件 | ncnn、FFmpegKit、MPP、libyuv、SNPE 等按构建开关与运行时条件启用，不能仅凭目录或头文件存在判定有效 |

OpenCV 路径须指向**源码根**，不是安装目录。Windows ncnn 须与项目静态 CRT 匹配；Android ncnn 必须使用对应 ABI 的 SDK。具体开关、来源与许可分别见[构建注意事项](AGENTS.md#essential-build-quirks)和 [CREDITS](CREDITS.md)。

## 快速开始

以下命令从仓库根目录执行。Windows 原生构建使用具有 VS2022 工具链的开发者 PowerShell；先按[构建与测试](docs/guides/build-and-test.md)准备工具和依赖。

### 构建 C++ 核心快通道

仅构建与运行不依赖 OpenCV 的核心测试：

```powershell
cmake --preset default
cmake --build --preset default --target core_unit_tests
ctest --preset default -R '^core_unit_tests$' --output-on-failure
```

此快通道不验证完整推理、相机或设备。Windows 服务及完整测试另见[选定目标构建](docs/guides/build-and-test.md#windows服务与选定测试powershell)，不要只构建服务后运行未筛选的全体 CTest。

### 构建 Android 桩通道

```powershell
.\gradlew.bat --no-daemon -PRK_SKIP_OPENCV=true :app:assembleDebug :app:checkApkFaceModels :app:testDebugUnitTest :app:lintDebug
```

该命令用于显式桩构建，不能验收真实识别。真实双 ABI 后端、模型导入与设备测试的前提见 [Android 构建](docs/guides/build-and-test.md#android)和[设备验收](docs/guides/rk3288-device-acceptance.md)。生产 Java 位于 `src/java/`，Java 单测位于 `tests/unit/java/`，不是 `app/src/test/`。

### 构建 Web

确认 `pnpm --version` 为 10.33.0，再执行：

```powershell
pnpm -C web install --frozen-lockfile
pnpm -C web lint
pnpm -C web test
pnpm -C web build
```

产物输出到 `src/win/app/webroot/`，**受 Git 跟踪**。开发服务器默认将 `/api` 代理到 `http://127.0.0.1:8080`，可由 `VITE_DEV_PROXY_TARGET` 覆盖；Vite 启动不代表 Windows 服务已经启动。

已有依赖目录若由其他 pnpm 版本生成，先核对目录与 store 兼容性，不自动删除或重建用户原有 `node_modules`。

## 测试与质量检查

C++ 同时使用 GoogleTest 和自定义 `bool` 测试，`win_unit_tests` 为混合目标；以[开发指南的框架归属](DEVELOP.md#43-测试框架)为准。新增用例须沿用所在目标的框架并注册到真实运行器。

Web E2E 需要匹配版本的 Cypress 二进制和已就绪、带插桩的 Vite 服务器。Windows 可执行：

```powershell
pnpm -C web exec cypress install
pwsh -NoProfile -File scripts/run-web-e2e.ps1
```

驱动及手工双终端步骤见 [Cypress 与覆盖率](docs/guides/build-and-test.md#cypress-e2e与覆盖率)。API mock 的 E2E 不代表真实后端验收，构建产物或 preview 服务器不能替代覆盖率插桩服务器。

文档修改至少运行：

```powershell
node --test tests/scripts/docs-sync-audit.test.js tests/scripts/docs-sync-audit-gate.test.js
node scripts/docs-sync-audit.js --skip-links
```

文档门禁要求 high=0、medium=0；该命令仍检查仓内引用，跳过的是外部链接探测。部分 C++ 结构测试在模型缺失时仍计 PASS，独立精度程序也可能报告 UNVERIFIED。**跳过、结构通过和合成回归都不等于真实人脸精度通过。**

## CI 流水线与合并

[主 CI](.github/workflows/ci.yml)包含变更分类、仓库卫生、核心测试、文档、Web、Android 和 Windows 检查；另有[独立文档工作流](.github/workflows/docs-audit.yml)。主 CI 支持面向 master 的 PR、master push 和手动触发，Windows job 不在手动触发时运行。

纯文档变更保留分类及文档门禁，其余主 CI job 按条件跳过。Linux 核心测试、Web E2E/覆盖率及部分设备无关辅助步骤为既有非阻断信号，须人工查看结果，不能由 job 绿色推断全部检查通过。具体门禁边界见 [CI 说明](AGENTS.md#ci-pipeline)。

合并前须处理审查意见、核对冲突和必需检查；仅使用 squash and merge。提交、推送、创建 PR 与实际合并分别遵循用户授权，Draft PR 不自动获得合并授权。

## 许可证与模型限制

仓库自有代码采用 [MIT License](LICENSE)，**不代表整个部署包及模型权重都采用 MIT 或可直接商用**。

- 当前主路径使用的 InsightFace 检测/识别权重及其量化派生产物仅限非商业研究；实际来源、限制与替代候选见 [模型台账](CREDITS.md)。模型二进制不随仓库分发，APK 也不得内置人脸模型。
- 本地可选 FFmpegKit 6.0.LTS / x264 AAR 为 GPL 3.0 或更新版本，消费位置固定为 `app/libs/ffmpeg-kit.aar`。来源、摘要、构建方式和再分发边界见 [FFmpegKit 工具说明](scripts/ffmpegkit/README.md)及[供应链账本](docs/security/supply-chain-ledger.md)。不需要推流时可以不提供该 AAR。
- 第三方库、BSP 快照与其他模型各有许可；统一入口为 [CREDITS](CREDITS.md)。不将功能测试或用户用途声明作为许可证明。

安全问题的报告方式见 [SECURITY.md](SECURITY.md)。历史审计与治理记录从[文档索引](docs/index.md)进入，其评分、完成状态和测试结果均有时点边界，不作为当前全部能力已验收的声明。

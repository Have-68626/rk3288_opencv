<!-- DOCSYNC_DOC
type=design
status=living
as_of=2026-09-16
-->

# 模型获取与导入流程（Android 端）


**状态**: 已落地。承接 P1-6：模型不随 APK 分发，由用户自行获取并经 ZIP 导入，`FaceModelManager` 负责校验与原子启用。

**范围**: Android App 的模型导入链路（`src/java/com/example/rk3288_opencv/FaceModelManager.java`、`FaceModelManifest.java`、`NativeFaceBridge`）。

## 1. 合规与分发边界（必读）

- **APK 不内置模型**：默认构建会清理并禁止 `assets/face_models/`（`app/build.gradle` 的 `checkApkFaceModels` 在 APK 中出现该目录即构建失败）。仓库仅跟踪 `models/android/<profile>/` 下的 `manifest.json`、`LICENSE.txt`、`NOTICE.txt`，`*.param` / `*.bin` 被 gitignore。
- **用户自行获取、自行导入**：模型由使用者获取并经应用内文件选择（SAF）以 ZIP 导入。
- **合规自负**：`models/android/rk3288_light/LICENSE.txt` 明确“本仓库不随 APK 分发模型权重——模型由使用者自行获取并导入、自负合规责任，本项目仅提供导入与校验机制”；上游训练权重限非商业研究用途，再分发须自行取得 InsightFace 明确许可。
- **许可门禁**：导入前必须 `licenseAccepted==true`，否则返回 `LICENSE_NOT_ACCEPTED`（“请先确认模型许可后再导入”）；profile 的 `licenseId` 会随运行时状态暴露。此外，每个导入模型的**来源 / 许可证 / 是否可商用**须在 manifest 中登记（见 §3.1）。

## 2. 模型获取与格式

- 内置两个参考 profile（`models/android/`）：`rk3288_light`（SCRFD 检测 640×640 + ArcFace 112×112，512 维，支持 `armeabi-v7a`/`arm64-v8a`）与 `arm64_accuracy`（高精度，用于 ARM64 验收）。
- 检测模型为 SCRFD、识别为 ArcFace/MobileFaceNet，运行于 ncnn/CPU；RK3288 无 NPU，不得引入 rknn/rknpu 依赖。
- 模型须为 ncnn 的 `.param` + `.bin`（非 Git LFS pointer），并由导入方在 manifest 中填 `sha256` 引脚。

## 3. ZIP 导入契约

- ZIP 内**有且仅有一个** `manifest.json`；其所在目录即模型基目录，其余 `param`/`bin` 相对该目录。
- 文件数 ≤ 64；解压累计字节 ≤ 256 MiB；`manifest.json` 体量 ≤ 1 MiB。
- `manifest.json` 必填/关键字段（示例见 `models/android/rk3288_light/manifest.json`）：
  - `schemaVersion` 必须为 `1`。
  - `supportedAbis`：必须包含设备当前 ABI，否则 `ABI_MISMATCH`。
  - `profileId`：可作目录名的安全串，且不得以 `.` 开头、不得为保留名 `active_profile`。
  - `detector.param` / `detector.bin`、`embedder.param` / `embedder.bin`：安全相对路径，`sha256` 可为空（空即跳过该项校验）。
  - `detector.outputs`：若给出必须恰好 9 个（SCRFD 多尺度头）。
  - 其他：`displayName`、`inputWidth/Height`、`decodeMode`、`anchorsPerPos`、`outputLayout`、`embeddingDim`、`detectionConfidence`、`nmsIou`、`identitySimilarity`、`modelVersion`、`licenseId`。
- 单 profile 的目录形态：
  ```
  face_models/
    active_profile            # 当前激活 profileId（纯文本指针）
    active_profile.tmp/.bak   # 指针原子写的临时/备份
    <profileId>/manifest.json
    <profileId>/scrfd.param / scrfd.bin
    <profileId>/arcface.param / arcface.bin
    .tmp_<nano>/              # 解压临时目录（下次导入时清扫）
    .bak_<profileId>/         # 目录替换时移开的旧配置备份
    .txn_<profileId>          # 目录替换**事务记录**（任务 A，见 §4 步骤 7/8）
    .rollback_<profileId>     # 旧版回滚意图标记（已非主路径，仅保留兼容与恢复）
  ```

### 3.1 许可证与商用限制登记（必填）

导入一个模型时，manifest 必须**显式登记**该模型的**来源、许可证与是否可商用**——这三项与 `licenseAccepted` 门禁（§1）共同构成合规边界；缺失或含糊者视为登记不完整：

- `source`：权重来源（可核对的 URL 或上游项目名，如 `https://github.com/deepinsight/insightface`），不得留空。
- `license`：许可证名称，写**具体名称**（SPDX 标识或全称，如 `MIT` / `Apache-2.0` / `BSD-2-Clause` / 专有）；无法核实的须显式写“未能核实”，不得臆测或留空。
- `commercialUse`：布尔值——`true` 表示可商用，`false` 表示不可商用。注意**代码许可证与权重许可证可能不同**：例如 insightface 系模型，其代码可能是 MIT/Apache-2.0，但训练权重声明“仅限非商业研究用途”，故 `commercialUse=false`。不可商用模型须同时在 profile 的 `LICENSE.txt` 中写明再分发边界。
- `licenseId`：（既有字段，随运行时状态暴露）指向 profile 内 `LICENSE.txt` 的许可标识，如 `insightface-models-noncommercial-research`。

**权威事实源**：模型来源 / 许可证 / 商用限制的台账见 [`CREDITS.md`](../../CREDITS.md) 的「模型台账与依赖」一节（含 `yunet`(MIT) 与 `sface`(Apache-2.0) 等可商用替代）。本流程只做登记，不改变既有 `licenseAccepted` 判定语义。

## 4. `FaceModelManager.importZip` 校验链（顺序即安全边界）

全程在 `synchronized (dirLock())` 内执行：

1. **入参/许可门禁**：stream 为空或 `currentAbi` 为空 → `INVALID_ARGUMENT`；`!licenseAccepted` → `LICENSE_NOT_ACCEPTED`；`ensureStoreDir()` 失败 → `IO_ERROR`。
2. **逐条解压到同盘临时目录 `.tmp_<nano>`**（`extractZip`）：
   - entry 名为空、含反斜杠、非安全相对路径 → `ZIP_SLIP`；
   - `fileCount > MAX_FILE_COUNT(64)` → `TOO_MANY_FILES`；
   - canonical 路径包含性兜底（逃出临时目录）→ `ZIP_SLIP`；
   - streaming 写出时累计 `> MAX_TOTAL_BYTES(256 MiB)` → `TOO_LARGE`（边写边判，不先落盘全量）；
   - 单条 `manifest.json` `> MAX_MANIFEST_BYTES(1 MiB)` → `TOO_LARGE`。
3. **唯一定位 manifest**：0 个 → `MISSING_MANIFEST`；多于 1 个 → `MANIFEST_INVALID`。
4. **manifest 校验**（`FaceModelManifest.parse`，与 native 契约一致）：schemaVersion==1、ABI、路径、sha256 格式；`ABI_MISMATCH` 单独暴露，其余归 `MANIFEST_INVALID`；`profileId` 再查安全目录名与保留名。
5. **模型文件校验**（`verifyModelFiles`）：canonical 包含性 → `MANIFEST_INVALID`；缺失 → `MISSING_MODEL_FILE`；Git LFS pointer（`version https://git-lfs.github.com/spec/v1`）→ `MISSING_MODEL_FILE`；SHA-256 与引脚不符 → `SHA256_MISMATCH`。
6. **native 校验/暂存**（`nativeValidateProfile`，原 `nativeSelfCheck`，P1-2 澄清语义）：调 `NativeFaceBridge.loadProfile` 做格式/ABI 校验，只看 `validated:true`；失败 → `NATIVE_CHECK_FAILED`。此步**只校验暂存，不等于加载生效**，旧目录与激活指针完全未被触碰。
7. **原子启用目录**（`swapInProfileDirs`）：**先在 `.txn_<profileId>` 落 `in_progress` 事务记录**（含 hadOld 与原激活指针；写不进去即中止，旧配置一动未动），再把基目录 rename 到 `face_models/<profileId>`（如有旧目录先 rename 为 `.bak_<profileId>`），同盘 rename 保证原子；**暂不**写激活指针。
8. **激活并落定**（`nativeActivateRuntime`）：调 `activateRuntime`，仅当返回 `ok:true` 且 `effective || reason=="pending"` 才算受理；失败 → `RUNTIME_ACTIVATE_FAILED`，**保留新目录但不切指针**（旧配置继续生效），绝不静默报成功。最后 `persistActive(profileId)` 原子落定指针；成功后把事务记录改写为 `committed` → 清理 `.bak_` → 清事务记录（**仅在备份确已清掉时才清记录**）。指针落定失败则 `rollbackProfileSwap` + `bestEffortReactivate(previousActive)` → `ATOMIC_RENAME_FAILED`。`finally` 清理临时目录。

**事务记录与崩溃恢复（任务 A）**：`.txn_<profileId>` 的 `state` 区分「替换进行中」与「已提交未清理」，恢复据此判定——`committed` ⇒ 保留新目录并继续清备份；`in_progress` + `hadOld` + `.bak_` 在 ⇒ 回滚旧目录**与原指针**；`in_progress` + `!hadOld` ⇒ 无事可做（新包保留、指针未改）。任一恢复步骤失败都**保持现场并保留记录**，状态报 ERROR 且**拒绝继续覆盖导入**；「无记录且新旧目录并存」按**歧义现场**处理（保留两份、不猜）。写入事务记录前保证 `.bak_` 已不存在，使「`in_progress` + `.bak_` 在」恒等于「本次替换已移开旧目录」。

`activate(profileId)` 对已导入目录重跑第 4–6 步校验后原子改写指针，失败时指针不变；目标未导入 → `PROFILE_NOT_FOUND`。

### 4.1 常量

| 常量 | 值 |
|---|---|
| `MAX_FILE_COUNT` | 64 |
| `MAX_TOTAL_BYTES` | 256 MiB（256×1024×1024） |
| `MAX_MANIFEST_BYTES` | 1 MiB |
| `MANIFEST_FILE_NAME` | `manifest.json` |
| `ACTIVE_POINTER_FILE` | `active_profile` |
| `DETECTOR_OUTPUT_COUNT` | 9 |
| Git LFS 特征串 | `version https://git-lfs.github.com/spec/v1` |

### 4.2 结果码

| 码 | 含义 |
|---|---|
| `OK` | 成功 |
| `NOT_IMPORTED` | 首启未导入（需引导 SAF 导入） |
| `INVALID_ARGUMENT` | 入参非法 |
| `LICENSE_NOT_ACCEPTED` | 用户未确认许可 |
| `ZIP_SLIP` | Zip Slip（绝对路径/`..`/反斜杠/canonical 逃逸） |
| `TOO_MANY_FILES` | 文件数超 64 |
| `TOO_LARGE` | 解压总量超 256 MiB 或 manifest 超 1 MiB |
| `MISSING_MANIFEST` / `MANIFEST_INVALID` | manifest 缺失/非法 |
| `ABI_MISMATCH` | ABI 不匹配 |
| `MISSING_MODEL_FILE` | 模型文件缺失或为 LFS pointer |
| `SHA256_MISMATCH` | SHA-256 引脚不符 |
| `NATIVE_CHECK_FAILED` | native 校验/暂存失败 |
| `RUNTIME_ACTIVATE_FAILED` | 实时管线激活失败（旧配置继续生效） |
| `IO_ERROR` / `ATOMIC_RENAME_FAILED` / `PROFILE_NOT_FOUND` | 其他受控失败 |

## 5. 未导入模型时的行为（禁止静默回退 LBP）

- 未导入/未激活时，Android 端应保持**降级预览**并显式报告 `missing_model`，**绝不静默回退 LBPH**。
- 代码强制点：`Engine.cpp` 注释与逻辑（“模型尚未加载时仅预览并显式 missing_model，绝不静默回退 LBPH”）、`RealtimeFacePipeline.cpp` 的 `failResult("missing_model", …)`、`native_face_bridge.cpp` 的 `getRuntimeStatus` 缺省 `reason="missing_model"`、`MainActivity` 显示“缺失原因=…”。
- `missing_model` 是加速契约固定 reason 码之一（`src/cpp/include/AccelerationContract.h`）。

## 6. 未覆盖项

- 导入 UI 的完整交互（许可确认、SAF 选择、进度、失败提示）**已接线**（任务 18）：`MainActivity.bindModelManagementUi` 绑定 `btn_import_model`（`:3497`）→ `confirmLicenseThenPickModelZip` → SAF 回选后 `importModelZip`（`:3638`）在后台调用 `FaceModelManager.importZip` 并展示成功/失败。
- 注册 UI 亦已接线（`btn_register_face`，`MainActivity:3500`），但注册闭环因缺 native 逐帧特征源仍不可用（Blocker，待补），详见 `docs/designs/android-enrollment-design.md` §3。
- 模型下载/自动获取不在本项目范围；本仓库不提供模型分发。
- 多 profile 并存时的磁盘占用与清理策略未定义（当前保留 `.bak_<profileId>` 以便回滚）。

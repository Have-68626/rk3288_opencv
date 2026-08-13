# CivetWeb 来源账本（Source Provenance Ledger）

治理任务 **SEC-17（S-SEC-B10-008）**。本目录为 CivetWeb 第三方源码与许可证归档快照，**未编入当前构建目标**（Windows 本地服务使用自有的 `HttpFacesServer` 实现，不引用本目录；仅作归档与许可证合规保留，见 `CREDITS.md`）。

本账本用于保证：来源可追溯、文件摘要可复算、本地差异有记录。

## 上游来源

- 仓库：`civetweb/civetweb`（<https://github.com/civetweb/civetweb>）
- 版本：**1.16**（`include/civetweb.h` 声明 `CIVETWEB_VERSION "1.16"`）
- 上游 commit（v1.16 tag）：`d7ba35bbb649209c66e582d5a0244ba988a15159`
  - 核实方式：GitHub API `GET /repos/civetweb/civetweb/git/refs/tags/v1.16` 返回 lightweight commit tag，指向上述 SHA。
  - 一致性证据：本地 `include/civetweb.h` 与该上游 commit 逐字节一致（SHA-256 相同）；本地 `src/civetweb.c` 与上游该 commit 的差异仅由下方 4 个本地补丁构成（补丁行数合计 +149/−25，与上游 diff 统计完全吻合，无其他未记录差异）。

## 文件清单与 SHA-256

**规范哈希 = git blob（LF 归一化入库字节）**，与 checkout 行尾状态无关（仓库 `.gitattributes` 对本目录 `text`/`eol` 生效，`core.autocrlf=true`）。
复算命令：`git cat-file blob HEAD:src/win/third_party/civetweb/<路径> | sha256sum`

| 文件 | SHA-256（git blob） | 相对上游 v1.16 | 说明 |
|:-----|:-----|:---|:-----|
| `include/civetweb.h` | `e3a12c0a659b3bc6f75aa67799232ca343572dcb3add8b2475666633fa4afad3` | 逐字节一致 | 上游原样（Pristine） |
| `src/civetweb.c` | `74f777048068dea7f12ac3de62a8fdbef97e6544647875693102e6689a4214e3` | 本地补丁 +149/−25 | 4 个安全补丁，见下表 |
| `src/md5.inl` | `03fe15af6f6c64272534ccd4e2abe6f7129bf165a4acd9e42bb7c2f9ed8b38eb` | 本地精简变体 | 175 行 vs 上游 472 行，见下 |
| `LICENSE.md` | `b5db2dc4706611a04a6ca0926d4c2b8389bd13026becf65b7f5d38bec52ab075` | 本地文件 | 头部为本仓库集成说明；正文为上游许可证正文（原样保留） |

## 本地差异（补丁集）

### `src/civetweb.c` — 4 个安全补丁（均已在仓库历史中）

| 本地 commit | 内容 | 行数 |
|:---|:---|:---|
| `997197f` | CORS origin 校验：新增 `cors_origin_matches`/`cors_origin_allowed`，接入 `send_cors_header` 与 CORS preflight（OPTIONS）处理 | +138/−9 |
| `85879e2` | 默认 `listening_ports` 由 `"8080"` 改为 `"127.0.0.1:8080"`，避免默认绑定所有网卡 | +1/−1 |
| `93ccc06` | `access_control_allow_origin`/`_methods`/`_headers` 默认值由 `"*"` 改为 `NULL` | +3/−3 |
| `4b4e0e2` | `delete_file` 权限检查重构：移除删除前 `access(W_OK)` 检查，改为依据删除结果 `EACCES`/`EPERM` 返回 403（CodeQL 告警加固） | +7/−12 |

合计 **+149/−25**，与"上游 v1.16 ↔ 本地文件"的 diff 统计完全一致（已核）。

### `src/md5.inl` — 本地精简变体

- 上游 v1.16 的 `src/md5.inl` 为 **472 行**（含 zlib 许可证头与 L. Peter Deutsch 署名注释）；本地为 **175 行**精简版本。
- 保留相同 API（`md5_state_t` / `md5_init` / `md5_append` / `md5_finish` / `md5_process`）与 RFC 1321 常量，函数行为等价。
- **剥离了上游 zlib 许可证头与 L. Peter Deutsch 署名注释**，未按 zlib 许可证条款 2 标注为修改版本 —— 合规隐患在此记录，本任务不改动该文件。
- 自初始导入 commit `ec50bae` 起未再变更；`CREDITS.md` 将该实现记录为 L. Peter Deutsch / zlib。

## 许可证来源确认

- 许可证正文见 `LICENSE.md`（上游原样保留；本账本任务**未改动任何作者/版权信息**）。
- CivetWeb 本体：MIT（`Copyright (c) 2013-2021 The CivetWeb developers` 等，见 `LICENSE.md`）。
- `src/md5.inl`：zlib（L. Peter Deutsch 的 MD5 实现，`CREDITS.md` 记录；本地文件未内嵌许可证头，见上）。

## 维护说明

- 任何对 `src/win/third_party/civetweb/` 的改动（含新增补丁）都必须同步更新本账本中的 hash 与补丁记录。
- 验证：`node --test tests/scripts/civetweb-ledger.test.js`（CI 已通过 `node --test tests/scripts/*.test.js` 目录级接入）。
- 哈希约定同 `tests/scripts/manifest-anchor-consistency.test.js`（git blob 为规范值，行尾无关）。

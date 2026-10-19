<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-10-18
-->

# 文档整改实施与验收

## 范围与结论

依据2026-10-18全仓文档审计的实施顺序，已完成D01—D10关联段落修订、M1—M6事实源收敛和五份已结束轮次的快照化。仅修改文档；没有修改业务代码、构建或CI配置，没有提交、推送或合入master。

执行基线为 `265d3fc575d649f2ff908bfdfc20a4f086e596fc`。原审计、逐文件清单及本轮细节证据分别存于本地忽略目录 `cache/reports/documentation-audit-2026-10-18/` 和 `cache/reports/documentation-remediation-2026-10-18/`；这些缓存不是长期操作入口。

## 十项修订与验证依据

| 项 | 落地内容 | 依据与验收边界 |
|---|---|---|
| D01 | Windows服务构建与选定测试分开，CTest只筛选已要求构建的四目标 | 对照CMake注册，并从现有构建树枚举CTest，四项集合相等；未在新目录重编产品 |
| D02 | E2E补齐服务器、127.0.0.1:5173、服务端插桩及独立覆盖率门槛 | 对照驱动、Vite、Cypress配置；本轮未启动浏览器 |
| D03 | INT8改为唯一同stem配对、table.json绑定、离线几何及四文件摘要 | 对照实际量化脚本及CLI帮助；未运行量化或精度验收 |
| D04 | Android注册说明集中定义空ID兼容、非空空间、显示名同空间及重新登记边界 | 对照仓库工厂、MainActivity路由和显示名消费者；未访问设备或真人数据 |
| D05 | BSP导出步骤改指现存证据登记 | 不再要求更新不存在的Release Note；未补造厂商证据 |
| D06 | Loop合并检查以明确人工授权为前提 | 权限引用loop-constraints，不因检查通过自动获得提交、推送或合并权限 |
| D07 | FFmpegKit区分原消费件局域网功能记录与完整实机未验收 | 引用原实测与供应链；后续候选不能继承旧包结果 |
| D08 | 前期PR轮转snapshot，并链接后续Cypress、Android专项和总账本 | 原失败、历史数字及总账本活跃状态保持 |
| D09 | PowerShell改用显式相对Gradle入口，POSIX独立列示 | 当前目录命令发现与9段PowerShell示例语法解析通过；未执行Gradle |
| D10 | 区分Engine实时路径和runFaceInferOnce单图入口 | 对照真实声明；旧类计数失效处标为待复核，未宣称接口契约闭环 |

## 事实源与入口

| 组 | 当前维护位置 | 其他入口与保留边界 |
|---|---|---|
| M1 构建与测试 | [构建与测试](../../guides/build-and-test.md) | README、DEVELOP、AGENTS/CLAUDE及角色说明引用；助手受控镜像保留 |
| M2 Android注册与导入 | [Android注册设计](../../designs/android-enrollment-design.md)、[模型导入](../../designs/model-acquisition-import.md) | 架构和顶层人员设计引用布局，导入事务与目标能力各保留原职责 |
| M3 INT8操作 | [INT8量化操作](../../guides/int8-quantization.md) | 设计保留运行时差异，供应链负责父链/许可，不把结构跳过当精度通过 |
| M4 FFmpegKit | [构建工具](../../../scripts/ffmpegkit/README.md)、[供应链](../../security/supply-chain-ledger.md)、[实测反馈](../2026-10-17-rk3288-runtime-feedback.md) | 构建计划保留原消费件历史命令，CREDITS只摘要并回链 |
| M5 存储与迁入 | [存储布局](../../guides/repository-storage-layout.md)、[审计工具](../../guides/audit-tools.md) | 旧计划链接结果与当前工具，保留manifest、SHA和历史路径绑定，不迁移原证据 |
| M6 Loop职责 | [约束](../../../loop-constraints.md)、[预算](../../../loop-budget.md)、[安全检查](../../safety.md) | 固定入口保留；STATE/run-log仍只承担运行态，不启动Loop或改写其状态 |

[索引](../../index.md)增加“当前操作／目标设计／历史证据／工具配置”阅读路径。长篇研究、模型LICENSE/NOTICE、机器可读契约及工具固定入口保留，不按篇幅或文件短小判定冗余。项目外个人记忆和客户端加载配置不在本次修订范围。

## 历史轮次处理

以下五份改为snapshot并补接续入口，原测试数字、失败原因和未验收声明不重写：

- 前期PR495审查修复轮。
- Android ncnn构建接线专项。
- Cypress启动与覆盖率专项。
- 外部Temp复用清理执行轮。
- 辅助工具整改报告。

PR495总整改、FFmpegKit后续消费验收、缓存工具任务5及存储整理的master合入仍保留未决边界，不因文档整理而关闭。

## 两份短文档退出状态

`docs/research/appendix-h.md`的“尚无阶段执行结论”已并回DEVELOP附录H并保留原锚点；`docs/security/civetweb-ssl-note.md`的当前适用性与未来验证要求已并入CivetWeb账本，正文回链已更新。

用户明确批准后，重新核验冻结清单、两文件大小/mtime/SHA及备份ZIP逐项SHA/CRC，使用安全执行器仅删除这两个普通文件，共1,356逻辑字节，并移除对应两条索引；没有删除目录或清理其他缓存。执行日志记录status=deleted、deleted_files=2、deleted_bytes=1356，备份与冻结清单保留。仓外旧直接链接会失效；恢复须从核验ZIP原位还原且不覆盖新文件，再反向应用本批索引补丁。不将逻辑字节数等同于磁盘净释放空间。

## 验收与限制

- 文档门禁修正后退出0，H0/M0/L10；保留首次失败证据，不改变检查规则。
- 文档同步、门禁、CivetWeb及供应链账本四套回归合计93项通过，无失败或跳过。
- 两指南9段PowerShell代码块解析错误为0，显式Gradle入口可发现；这是语法/命令发现核验，未证明依赖齐全或全部命令执行成功。
- Windows四个示例构建目标与CTest筛选集合相等；只枚举现有树，未声称新树构建或产品测试通过。
- 量化CLI帮助及生产模板路径/符号对照通过；未读取原始日志、私有媒体或个人记忆正文。

既有10项low信号维持人工分类：历史Removed项、上游源码引用及按docs相对语义存在的路径，不为清零恢复旧文件。外部URL未批量在线验证；本轮文档验收不替代全量CI、浏览器E2E、设备推流或真人精度验收。

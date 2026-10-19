<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-10-19
-->

# 旧构建与历史缓存退出记录

## 边界与授权

基线为ca190d79065a0c42e20c79d8727f001e5025c703。用户要求尽可能退出原始日志、唯一历史证据和过时构建，确认相关使用者停止，并批准冻结的48目录、9个cache根文件清单。该授权不扩大到当前构建、正式模型、消费AAR、外部工具链或未列明的其他缓存。不提交、不推送，不操作实机。

## 冻结与执行

批准范围为75,683个普通文件、7,719,190,643逻辑字节，另有1,027个重解析节点及49个只读文件。目录清单SHA256为e676795fe087eb8df264080ccb26d0c0d1a384e64c914664c137fd53692776c7，根文件清单SHA256为5682de3760e8386f6e825ad5ba65f8bc8e62679b63f79eebd9f5d4fc8ca25f58。

目标包括build_android_rk3288、build_decision_core、build_fp15_gtest，cache内旧审计/构建/转换中间件、旧Cypress副本、历史reports、推流原始材料和交接检查点。冻结全体文件集合、大小、mtime与摘要，干跑及27项安全回归通过后才进入实际执行。链接仅退出节点，不进入目标；只读文件确认不与保留文件共享后才清属性。

两批实际结果均为deleted：目录批75,674个普通文件、7,719,155,821逻辑字节，根文件批9个文件、34,822字节；合计75,683个文件、7,719,190,643逻辑字节（约7.19 GiB）。另退出1,027个重解析节点，并清除清单内49个文件的只读属性。计数与冻结清单一致，48目录与9个根文件均核验不存在。

目录批执行窗口的卷空闲量由218,797,428,736增至226,171,887,616字节，增加7,374,458,880字节。这是已复制必要输入之后的瞬时观测，不是整轮净收益；受硬链接、分配粒度及其他进程影响，不能与逻辑删除量等同。

## 必要输入及保留

| 内容 | 当前位置 |
|---|---|
| Windows当前构建 | cache/builds/windows-vs2022-x64-ncnn |
| Android真实构建 | cache/builds/android-native-real/Debug/1o35ad6g及android-gradle-real/app；仅退出1gh2c4h1失败分支 |
| mock现存输出 | cache/builds/android-gradle-mock、android-native-mock；历史清单APK在删除前已不存在，不声称本轮mock验收通过 |
| 固定GTest、GNU config | cache/dependency-sources/googletest/58d77fa8、gnu-config/805517123cbf |
| FFmpegKit固定Git输入 | cache/dependency-sources/ffmpegkit-6.0-lts下FFmpeg、x264、cpu-features、OpenSSL的提交前缀目录 |
| Java源码及许可 | cache/dependency-sources/smart-exception/0.2.1 |
| NDK25离线包 | cache/tools/android-ndk/android-ndk-r25c-linux.zip |
| Android隔离构建配置 | cache/tools/android-layout/{real,mock}-output.gradle |
| 原始FFmpegKit ZIP | cache/ffmpeg-kit-6.0.LTS.zip |
| 模型及父图 | models、deps/insightface原位保留 |
| AAR消费件 | app/libs/ffmpeg-kit.aar原位保留 |

FFmpegKit四份Git输入为独立裸克隆，无alternates和原仓硬链接；提交及fsck核验通过。源码JAR和离线包逐项摘要匹配；NDK包内版本为25.2.9519653。本轮只保留重建输入，不宣称重新编译AAR。迁置清单中的新保留输入为574,472,922逻辑字节，另有少量映射与当轮验证元数据；不能把删除逻辑字节直接当净释放量。

正式Cypress缓存位于D:/ProgramData/CypressCache；已验证其13.17.0缓存可用，退出仓内旧副本。web/node_modules内另一缓存不在本次范围。本轮当前证据保留在cache/reports/legacy-build-retirement-2026-10-19，正式结论由本页维护，不把忽略目录当永久资料库。

## 验证

清理前Windows重新配置及14目标构建通过，CTest5/5通过；Android真实双ABI构建、401项Java单测零失败、lint和APK无模型断言通过，APK真实ncnn构建绑定与FFmpegKit代码匹配通过。Web lint零错误（3项既有覆盖率脚本警告）、162项单测及生产构建通过；Cypress verify通过。45项既存工作区文件的存在状态与摘要核验通过，HEAD及空暂存区不变。

删除后、重建前，240个必要输入文件、现存构建产物身份、4份固定镜像的提交及完整fsck均复核通过；此前未提交的非本轮改动、HEAD和空暂存区保持不变。

| 清理后检查 | 实际结果 |
|---|---|
| Windows重新配置、14目标构建 | 通过；现存Windows产物摘要保持不变 |
| Windows最终短临时路径CTest | 5/5通过，133.61秒；保留前两轮失败，不能据最终通过声称没有稳定性问题 |
| Android真实双ABI | assembleDebug、强制重跑401项单测、lintDebug、checkApkFaceModels全部通过，失败/错误/跳过均为0 |
| APK静态及构建绑定 | 双ABI、ncnn构建绑定、FFmpegKit代码匹配通过；不构成设备验收或签名检查 |
| Web lint、单测、生产构建 | 零lint错误（3条既有覆盖率脚本警告）、162/162单测及build通过 |
| Cypress完整E2E | 34/34通过，覆盖率门槛通过；语句90.53%、分支85.09%、函数88.14%、行94.16% |
| 文档审计 | H2/M0/L10；两项high均为既存侧边基准稿的非法status和索引缺失，本轮没有新增缺陷 |
| Git与保留内容 | 必要输入摘要及固定镜像核验通过；既存非本轮改动保留，无提交/推送 |

Android的BuildConfig每次配置写入BUILD_TIME（app/build.gradle），所以重新组装的APK整包SHA不同；前后静态检查报告除apk_sha256外完全一致，原生代码绑定及消费AAR摘要相同。新APK身份单独登记，未覆盖清理前摘要。mock历史APK清理前已缺失，本轮不补造或宣称mock通过。

Cypress启动器本次重新创建cache/web-e2e，按既有逻辑归档上一轮覆盖率。这是本次工具产生的新归档路径，冻结的旧归档文件未恢复；不将目录再生算作旧内容未删。

## 验证异常与待办

首轮清理后CTest为4/5：win_unit_tests中pipeline_stale_command_does_not_run_after_reload_wait失败。随后隔离复验该target通过，最终短路径全体CTest也通过；具体失败条件未记录，不能仅凭复跑归因为调度，也不能宣称该稳定性缺口已修复。该用例使用独立临时数据库和仍保留的源码资产，测试程序身份不变，未发现读取本批退出目录的证据。建议后续增加失败条件诊断，再验证其调度稳定性；本轮没有扩大修改生产代码或测试断言。

第一次隔离复验另因本轮驱动临时标签过长，face_infer_unit_tests在gallery_space_isolation创建目录时异常退出。最小MSVC复现：248字符目录创建返回错误206，缩至237字符成功；失败现场的spaces父目录存在而64字符子目录未建立。已将本轮驱动的CTest临时目录缩短为当轮证据根下tmp，最终相同5项CTest全部通过，没有修改生产配置、代码或测试门槛。

文档门禁两项high所属文件为docs/plans/2026-10-19-cyber-cricket-arena-benchmark-spec.md，按此前约定继续由侧边会话修订，未代改。知识图谱AST更新成功，但工具提示6个文件部分解析及社区标签需刷新；不将此作为构建或语义完整性验收。所有失败日志、退出码及最终结果分别保留，未用通过结果覆盖失败证据。

## 恢复边界

按用户明确授权，本批历史原始日志、唯一实测材料、未入库检查点、旧APK/二进制及旧回滚ZIP不另备份；删除后不能恢复原始正文或历史二进制身份。正式报告与SHA摘要不能替代这些原件。已入Git内容仍可回读，必要输入可支持当前构建重建，但不承诺恢复历史字节。

此前快照中的“保留”与“可从ZIP恢复”描述的是当时状态；本次退出以本页最终执行结果为准，不改写旧验收数字和失败结论。当前规则入口见[存储布局](../guides/repository-storage-layout.md)、[模型消费者](../guides/model-storage-and-consumers.md)及[FFmpegKit重建工具](../../scripts/ffmpegkit/README.md)。

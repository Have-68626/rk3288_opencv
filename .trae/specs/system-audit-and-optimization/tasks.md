# Tasks
- [x] Task 1: 资产清单梳理
  - [x] SubTask 1.1: 扫描并列出所有前端、后端、移动端、脚本、配置、文档、镜像、密钥、证书、云资源（含IAM、存储、网络、日志、监控）及其版本、责任人、最后更新时间。
  - [x] SubTask 1.2: 标记废弃、重复、未使用或缺失归属的资产。
  - [x] SubTask 1.3: 生成 `assets.csv` 文件。

- [x] Task 2: 代码质量诊断
  - [x] SubTask 2.1: 使用静态分析工具（如 ESLint, SonarQube 模拟等）对全量代码进行扫描。
  - [x] SubTask 2.2: 统计 Bug、漏洞、坏味道、重复率、测试覆盖率。
  - [x] SubTask 2.3: 对高危缺陷给出逐条定位、影响等级、修复建议与预计工时，并记录在报告中。

- [x] Task 3: 安全审计
  - [x] SubTask 3.1: 检测硬编码密钥、弱加密算法、过期证书、过度权限。
  - [x] SubTask 3.2: 检查依赖漏洞（CVE）、容器镜像漏洞、OSS许可证合规风险。
  - [x] SubTask 3.3: 输出风险矩阵（概率×影响）并给出修复优先级与复测方案。

- [x] Task 4: 性能与成本评估
  - [x] SubTask 4.1: 采集或模拟生产环境性能数据（CPU、内存、磁盘IO、网络延迟、数据库慢查询、API P99响应时间、错误率）。
  - [x] SubTask 4.2: 对比行业基线，定位性能瓶颈。
  - [x] SubTask 4.3: 统计云资源闲置与超配情况，量化可节省成本，生成 `costs.xls`（或类似格式）。

- [x] Task 5: 架构与可维护性评审
  - [x] SubTask 5.1: 评估模块化程度、耦合度、接口版本策略、回滚方案、灾备等级、文档完整度。
  - [x] SubTask 5.2: 识别单点故障、技术债、过时代码框架，并给出演进路线图。

- [x] Task 6: 报告整合与交付
  - [x] SubTask 6.1: 整合所有分析结果，编写 `ANALYSIS_REPORT.md`。
  - [x] SubTask 6.2: 确保报告包含执行摘要、风险总分与关键行动 Top10。
  - [x] SubTask 6.3: 整理附件（CSV资产清单、SARIF扫描结果、XLS成本明细）。

# Task Dependencies
- [Task 6] depends on [Task 1], [Task 2], [Task 3], [Task 4], [Task 5]

<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-18
-->

# 审查清单、评分与静态仪表盘

工具位于 `scripts/audit/`，从 PR495 历史审计脚本提炼，只用 Python 标准库，均显式指定输入输出。不执行构建、Git 修改、源码变异或台账更新；输出独占创建，失败返回2且不打印输入或异常正文。三项工具无需原来的 `D:/ProgramData/Temp` 工作区。

## 审查分派

```powershell
git -c core.quotepath=false ls-files | Set-Content -Encoding utf8 cache/tracked-files.txt
python scripts/audit/generate-review-map.py --files cache/tracked-files.txt --output cache/review-map.csv
```

输入是 UTF-8 一行一个、使用 `/` 的仓库相对路径。拒绝空清单、重复项、绝对路径、控制字符与目录穿越；含空格的路径正常保留。极少数含换行的 Git 文件名不支持此文本清单格式。保留原项目按路径与关键词划分模块、风险和任务的规则；`review_state=assigned`、空 `evidence_ids` 只表示待分派，**不是已经审查，也不是测试覆盖率**。上游代码和生成物的范围边界仍需人工复核。

## 评分

```powershell
python scripts/audit/score-audit.py --rubric cache/rubric.json --criteria cache/criteria.csv --baseline <40位提交SHA> --output cache/scorecard.json
```

`rubric.json` 必须显式给出规则版本、正整数满分、维度权重与准则编号，例如：

```json
{
  "version": "example-v1",
  "max_score": 4,
  "dimensions": [
    {"id": "D01", "weight": 100, "criteria": ["D01-1", "D01-2"]}
  ]
}
```

准则 CSV 必需列为 `criterion_id,dimension_id,weight,score,evidence_ids`；其他列可保留。维度权重必须为正且总和100，准则在所属维度内等权，CSV权重误差不超过百万分之一；重复/未知/遗漏准则、越界或非整数分数、NaN/Infinity、已评分项缺证据均拒绝。历史CSV可以省略不使用的尾列，不能省略必需字段。

空分数表示未知，不补造为0或通过。下界是已知项加权得分，未知项按0计下界、按满分计上界；`coverage` 为已知项的**加权证据比例**，不是测试覆盖率。`verified_score` 仅为已知部分归一化得分，全未知时为null；始终标记 `provisional=true`。工具校验“有证据编号”，不认证证据内容、规则选择或分数是否客观正确。基线SHA为调用者声明，工具不联网核验。

## 静态仪表盘

```powershell
python scripts/audit/generate-dashboard.py --scorecard cache/scorecard.json --findings cache/findings.jsonl --coverage cache/review-map.csv --output cache/audit-dashboard.html
```

读取评分JSON、发现JSONL和审查CSV，展示评分、发现与文件分派；不继承旧脚本的固定状态、日期或“当前已通过”声明。发现的必需字段为 `id,severity,status,title`，`impact` 可选。没有JavaScript、外部字体、CDN或网络请求；文本统一脱敏后HTML转义，并设置CSP。输出是输入快照，不替代审查人确认或项目CI。

脱敏覆盖常见URL账号口令、Bearer、password/token/secret/api_key、手机号与邮箱；不应故意输入真实凭据，也不把正则脱敏当成所有敏感格式的识别保证。完整原始日志和二进制不得因仪表盘生成而直接入库。

## 验证与历史材料

```powershell
python -m pytest tests/scripts/test_audit_tools.py -q
```

当前测试覆盖评分区间与非法数据、输出拒绝覆盖、路径约束、旧CSV兼容、转义与脱敏。历史材料位于本地忽略目录 `cache/reports/pr495/temp-2026-10-18/`，来源与脱敏摘要在其 `archive-manifest.json`。旧 `mutate.py`、摘要刷新、交接追加和台账关闭脚本仅保留历史，不属于正式工具。

<!-- DOCSYNC_DOC
type=runbook
status=living
as_of=2026-10-18
-->

# INT8量化操作

脚本入口为 `scripts/quantize_ncnn_int8.py`。设计与运行时限制见[INT8设计](../designs/int8-quantization-design.md)，父模型、许可及MobileFaceNet迁移口径见[供应链账本](../security/supply-chain-ledger.md#f32mobilefacenet-指定父链与迁移)。

## 输入前提

- 模型名仅 `scrfd`、`arcface`、`mobilefacenet`；输入目录名须包含对应模型族。
- 输入必须有且仅有一组同stem的 `.param` / `.bin` 配对。零配对或多配对拒绝，不按排序选第一对。
- ncnn2table/ncnn2int8须已在PATH、RK_NCNN_ROOT或NCNN_ROOT对应安装中可找到；SCRFD另需 `ncnn_calibration_images`，可用 `--calib-preprocessor` 指定。
- 完整流程或 `--table-only` 必须有校准图片。校准集与使用授权由操作者准备，合成回归不能冒充真人精度验收。
- 选新的独立输出目录保留旧结果。脚本不保证拒绝覆盖既有产物，不应把正式消费模型目录当试验输出。

## 完整流程（PowerShell）

```powershell
python scripts/quantize_ncnn_int8.py --model scrfd `
  --fp32-dir models/scrfd_ncnn `
  --calib-dir deps/WIDER_train/WIDER_train/images `
  --output-dir cache/model-conversion/scrfd-int8-new-run `
  --num-samples 50
```

路径为布局示例；运行前确认输入、工具和输出身份，不自动下载或替换正式模型。

## 分步校准与量化

同一命令加 `--table-only` 生成 `scrfd.table` 和同名sidecar `scrfd.table.json`；后续用相同父模型、预设和表执行 `--quant-only`，可用 `--table-path` 显式指定表。

sidecar绑定模型族、预处理预设、几何、父param/bin及表摘要。缺sidecar或与当前输入不匹配时拒绝量化，须重新校准，不能给旧表补造证据。

SCRFD先离线生成生产一致的居中零填充letterbox图片，再交给ncnn2table；几何记录为 `letterbox_center_pad0`，其他模型为 `resize`。输入尺寸与mean/norm来自脚本预设，不存在 `--size` 参数。更改预处理须同步模型/模板空间契约和重新校准，不在此流程自动迁移旧图库。

## 输出与报告

输出名为 `<model>_int8.param/.bin`。`quantize_report.json` 包含：

| 字段 | 语义 |
|---|---|
| fp32_bin_size、int8_bin_size、compression_ratio | 大小与压缩比 |
| fp32_param、fp32_bin、int8_param、int8_bin | 相对于报告目录的产物路径 |
| 上述四项对应的_sha256 | 父模型和INT8产物身份 |
| model、preprocessing、geometry | 本次模型族、归一化与几何契约 |

报告不提供真实检测精度或人脸识别准确率。保留报告与所绑定文件；仅比较压缩比不能验收模型。

## 运行时与精度验收

SCRFD脚本主线、历史yolo_face测试目录以及profile实际消费路径尚有差异，以[设计边界](../designs/int8-quantization-design.md)为准，不将生成模型直接视为生产已启用。先检查输入模型身份、实际后端和模板空间，再分别验证结构、合成契约、真实精度及设备性能。

结构用例的TEST_SKIP_ABSENT仍计PASS，独立精度目标的缺数据跳过也必须单列；任何跳过不等于精度通过。独立ncnn精度测试需要匹配静态CRT的ncnn/OpenCV，构建入口见[构建与测试](build-and-test.md)。

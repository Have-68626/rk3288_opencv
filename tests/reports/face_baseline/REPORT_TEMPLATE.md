# 人脸推理性能基线报告模板（可复现）

## 1. 基本信息

- 日期（ts_ms）：`<填写 face_baseline 输出的 ts_ms>`
- 设备：`RK3288 / Android 7.1.2 / CPU 频率策略 <填写> / 温控状态 <填写>`
- 构建：`rk3288_cli <版本/commit/构建类型>`

## 2. 复现命令

将下面命令整段复制到目标设备执行（建议先 `cd /data/local/tmp`）：

```bash
./rk3288_cli --face-baseline <imagePath|dir> \
  --warmup 5 --repeat 50 --detect-stride 1 --include-load 0 \
  --yolo-backend opencv --yolo-model <yolo.onnx> --yolo-w 320 --yolo-h 320 \
  --arc-backend opencv --arc-model <arcface.onnx> --arc-w 112 --arc-h 112 \
  --gallery-dir <gallery_dir> --topk 5 --face-select score_area \
  --out-dir tests/reports/face_baseline --out-prefix face_baseline
```

## 3. 输出文件位置

命令结束后，在设备侧查看输出：

- raw CSV：`tests/reports/face_baseline/<stem>_raw.csv`
- summary CSV：`tests/reports/face_baseline/<stem>_summary.csv`
- Markdown 报告：`tests/reports/face_baseline/<stem>.md`

## 4. 统计口径

- msDetect：人脸检测耗时
- msEmbed：特征提取耗时
- msSearch：1:N 检索耗时
- msTotal：单次循环总耗时（包含 detect/align/embed/search；是否包含 load 由 include_load 控制；不包含模型初始化与 gallery 加载）

## 5. 结果粘贴区（从 `<stem>.md` 复制）

将设备跑出的 `<stem>.md` 的“汇总（P50/P95）”表格粘贴到这里。


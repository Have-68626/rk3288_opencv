# 参数评估排名

- 输入: `tests\test_set01`
- cascade: `tests\data\lbpcascade_frontalface.xml`
- manifest: `tests\test_set01\manifest.csv`
- 汇总 CSV: `tests\reports\test_set01\grid\grid_summary.csv`
- 结果目录: `tests\reports\test_set01\grid\grid_runs`

| Rank | Tag | scale | neighbors | min | max | nms_iou | exact_accuracy | avg_abs_error | avg_ms_detect |
| ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | tuned_n8_min60_nms030 | 1.1 | 8 | 60 | 0 | 0.3 | 0.75 | 0.25 | 10.5 |
| 2 | tuned_n8_min60 | 1.1 | 8 | 60 | 0 | 0 | 0.75 | 0.25 | 10.5833 |
| 3 | tuned_n8_min60_nms040 | 1.1 | 8 | 60 | 0 | 0.4 | 0.75 | 0.25 | 10.75 |
| 4 | tuned_n8_min60_nms050 | 1.1 | 8 | 60 | 0 | 0.5 | 0.75 | 0.25 | 11.8333 |
| 5 | tuned_n8_min30 | 1.1 | 8 | 30 | 0 | 0 | 0.666667 | 0.333333 | 42.1667 |
| 6 | tuned_n8_min30_nms030 | 1.1 | 8 | 30 | 0 | 0.3 | 0.666667 | 0.333333 | 64.9167 |
| 7 | tuned_n6_min60 | 1.1 | 6 | 60 | 0 | 0 | 0.666667 | 0.416667 | 10.3333 |
| 8 | default | 1.1 | 3 | 30 | 0 | 0 | 0.333333 | 1.41667 | 34.4167 |

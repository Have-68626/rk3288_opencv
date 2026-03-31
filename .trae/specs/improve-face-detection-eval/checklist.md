# Checklist

- [x] `tests/test_set01/manifest.csv` 覆盖全部样本且标注口径正确
- [x] CSV/JSON 结果包含 `filename/has_face/faces/ms_detect/ok/err`
- [x] 存在标注时，CSV/JSON 结果包含 `expected_faces/match/abs_error`，summary 包含 `exact_accuracy/avg_abs_error`
- [x] 可视化输出可用：输出目录生成带人脸框的结果图
- [x] 去重/合并策略可用：开启后 faces 计数基于去重结果
- [x] 参数评估输出可用：生成参数排名汇总表
- [x] `scripts/verify_faces_test_set01.bat` 可一键复现并生成全部结果文件
- [x] `tests/reports/test_set01/face_report.md` 更新并包含默认/调优/去重对比结论与下一步建议
 

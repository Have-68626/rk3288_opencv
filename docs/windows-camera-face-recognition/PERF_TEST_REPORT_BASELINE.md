# Windows 摄像头人脸识别测试系统：性能测试基线（离线）

## 1. 说明
本基线用于在无摄像头环境下，提供“检测+特征+比对链路”的可复现性能参考。该数据不等价于真实摄像头端到端 FPS（采集与 Win32 绘制开销未计入）。

## 2. 测试环境
- 操作系统：Windows 10.0.26220 x64
- 编译器：MSVC 19.44（VS 2022）
- OpenCV：4.10.0（从源码构建，BUILD_LIST=core/imgproc/imgcodecs/objdetect）

## 3. 测试方法
使用 `win_face_bench_cli` 对目录图片进行重复识别，统计吞吐与内存占用：
- 输入：`tests/test_set01`（12 张图片）
- 检测器：`tests/data/lbpcascade_frontalface.xml`
- 迭代次数：50（总 600 帧）

命令示例：
```powershell
.\build_win_wcfr\bin\Release\win_face_bench_cli.exe `
  --input (Resolve-Path tests\test_set01) `
  --cascade (Resolve-Path tests\data\lbpcascade_frontalface.xml) `
  --iters 50
```

## 4. 结果
实际输出：
- `BENCH_SUMMARY images=12 iters=50 frames=600 faces=700 ms_total=6275.04 fps=95.617 avg_ms=10.4584 rss_bytes=108630016`

汇总（便于阅读）：
- 平均吞吐：约 95.6 FPS（离线识别）
- 平均耗时：约 10.46 ms/帧
- 进程工作集：约 103.6 MB


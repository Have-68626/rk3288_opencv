# Verification of Benchmark Timing Fix

## Tests Performed
- Downloaded `mobilenetv2-7.onnx` and `squeezenet_v1.1` models.
- Compiled `inference_bench_cli` with `RK_ENABLE_NCNN=ON` and `RK_SKIP_OPENCV=OFF`
- Ran OpenCV and NCNN benchmarks, measured pre-processing and pure inference times.

## Measurement Results
| 框架 | setInput/ex.input 开销 | 改进幅度 |
|------|-------------------|--------|
| OpenCV | ~0.48 ms | (16.45 - 15.96) ms |
| NCNN | ~0.75 ms | (0.76 - 0.005) ms |

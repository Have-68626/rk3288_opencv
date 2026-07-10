# Bolt Performance Memory

## Measurement First
- Benchmark the real production path before changing code. Do not count improvements on cold-path tooling as meaningful application wins.
- If a hotspot is not covered by an existing benchmark, write a temporary micro-benchmark, measure before and after, and clean it up immediately.
- Keep benchmark timing boundaries tight. Exclude setup work such as network input staging, extractor creation, or other one-time preparation when the goal is to measure the hot operation itself.

## Verified C++ Performance Patterns
- Prefer `std::partial_sort` when only Top-K results are needed.
- Use `float` accumulators in tight loops over `float` arrays unless double precision is truly required.
- Replace `std::stringstream` with pre-sized `std::string` concatenation in high-frequency event or logging paths when formatting is simple.
- Avoid redundant `cv::Mat::clone()` calls in read-only or conversion-light hot paths. Reuse buffers or shallow copies when lifetime and layout guarantees allow it.
- For NCNN image ingestion, prefer native pixel conversion such as `ncnn::Mat::PIXEL_BGR2RGB` over an extra `cv::cvtColor` pass.

## Scope And Safety
- Optimize code that affects actual application workflows first: inference, rendering, event handling, and frame processing.
- Keep optimizations readable and local. Do not trade maintainability for speculative micro-gains.
- When a change depends on layout assumptions such as `isContinuous()` or read-only ownership, document the reason and rollback path in the code near the optimization.

## Repo Hygiene For Perf Work
- Do not commit temporary benchmark scripts, generated binaries, or object files used only for local measurement.
- Keep performance memory in `.Jules/bolt.md` durable and pattern-based rather than tied to one transient task or measurement run.
## 2026-04-17 - Avoid cv::cvtColor for NCNN Pixel Conversion
**Learning:** Using `cv::cvtColor` to swap color channels (e.g., BGR to RGB) before passing a `cv::Mat` to NCNN
introduces redundant memory allocation and copy overhead per frame. This is a common bottleneck in tight inference
loops.
**Action:** Use NCNN's native pixel conversion flags (e.g., `ncnn::Mat::PIXEL_BGR2RGB`) within
`ncnn::Mat::from_pixels()` directly. Ensure the input `cv::Mat` is continuous (`isContinuous()`) before conversion to
avoid access violations.
## 2025-01-20 - YUV420 to I420 Conversion Pointer Optimization
**Learning:** Double loops with indexing and multiplications (`col * yPixelStride`) inside tight inner loops for YUV420
pixel extraction generate redundant instructions and prevent compiler vectorization, slowing down end-to-end frame
decoding times.
**Action:** Use sequential pointer arithmetic to traverse image rows and pixels, moving the pointer directly rather than
recalculating the memory offset on every iteration.

## 2024-05-18 - 优化帧处理与渲染中的内存分配
**Learning:** `cv::Mat::clone()` 强制执行深度内存拷贝并分配全新内存块。如果在热门的处理循环（如
`Engine::processFrame`、`FramePipeline::processLoop`）中频繁使用，会导致持续的堆内存分配、较高的内存碎片率以及潜在的 GC 抖动（在跨 JNI 或大量小对象分配时尤为明显）。
**Action:** 使用预先分配的/持久化的缓冲矩阵（如类成员或被置于循环外部的 `drawBuffer`/`frameBuffer`），并配合
`cv::Mat::copyTo()`，从而仅执行数据覆盖而无需重新分配，这在保证同样线程安全性的同时显著降低了动态分配开销。

## 2024-05-25 - Avoid deep copy for read-only D3D11 texture uploads
**Learning:** Uploading multi-channel frames like `CV_8UC4` to Direct3D 11 textures in `src/win/src/D3D11Renderer.cpp`
is a read-only operation from CPU memory. Performing an explicit `bgr->clone()` creates an unnecessary deep copy of the
image memory, causing repetitive multi-megabyte heap allocations and high memory bandwidth overhead per frame.
**Action:** Use a shallow copy (`bgra = *bgr`) when capturing the frame pointer for D3D11 upload if the source is
already continuous and formatted correctly (e.g., `CV_8UC4`).

## 2026-05-01 - Avoid Redundant deep copy when uploading cv::Mat to D3D11 Texture
**Learning:** Uploading a `cv::Mat` frame to a Direct3D 11 texture (e.g., in `D3D11Renderer.cpp`) is inherently a
read-only operation. For multi-channel frames that do not require layout conversion (like `CV_8UC4`), using
`cv::Mat::clone()` creates a redundant deep copy of the image, leading to excessive memory allocation (e.g., ~8.3MB per
1080p frame). This causes continuous RSS bloat, high memory bandwidth usage, and latency jitter during the render phase.
**Action:** Use a shallow copy (e.g., `bgra = *bgr`) instead of `clone()` for read-only `CV_8UC4` frame uploads to
eliminate per-frame allocations, while still safely incrementing OpenCV's reference counter to keep the buffer alive.

## 2026-05-07 - Optimize EventManager JSON formatting
**Learning:** `std::stringstream` has significant overhead for simple string concatenation due to virtual function
calls, locale handling, and dynamic memory allocations. In high-frequency logging/event pipelines, this becomes a
bottleneck.
**Action:** Replace `std::stringstream` with `std::string`, use `.reserve()` to pre-allocate sufficient capacity, and
use `operator+=` for concatenation to avoid reallocation and virtual function overhead, leading to measurable
performance gains.
## 2026-05-18 - Optimize Engine::processFrame string formatting
**Learning:** `std::ostringstream` has significant overhead for simple string concatenation due to virtual function
calls, locale handling, and dynamic memory allocations. In high-frequency rendering loops (`Engine::processFrame`), this
becomes a bottleneck.
**Action:** Replace `std::ostringstream` with `std::string`, use `.reserve()` to pre-allocate sufficient capacity, and
use `operator+=` for concatenation to avoid reallocation and virtual function overhead, leading to measurable
performance gains.
## 2026-05-24 - Optimize memory ops in loop
**Learning:** Element-wise loops on std::vector buffers can often be safely and elegantly replaced with std::copy,
combined with CV_Assert for size validation, yielding small performance improvements without losing type safety.
**Action:** Use std::copy for linear float array duplication instead of row/col indexing where continuous buffer
constraints hold true.

## 2024-05-18 - C++ Mat Clone Bottleneck
**Learning:** In the Windows local server rendering path (`src/win/src/FaceRecognizer.cpp`), `cv::Mat::clone()` was
being unnecessarily called on read-only regions (like cropping for inference). Using the direct Region Of Interest (ROI)
when resizing/converting cuts down on heap allocations.
**Action:** Always verify if a `clone()` is actually necessary when dealing with `cv::Mat` objects inside per-frame
inference loops. Pass direct ROIs to OpenCV functions instead, unless structural separation is explicitly required.

## 2026-05-19 - Optimize StructuredLogger and RenderMetricsLogger formatting
**Learning:** `std::ostringstream` has significant overhead for string concatenation due to virtual function calls,
locale handling, and dynamic memory allocations. In logging loops (`StructuredLogger::append`,
`RenderMetricsLogger::append`), this creates unnecessary CPU and memory overhead per frame log.
**Action:** Replace `std::ostringstream` with `std::string`, use `.reserve()` to pre-allocate memory, and use
`operator+=` for concatenation and `snprintf` for doubles to avoid reallocation and virtual function overhead, leading
to measurable performance gains.

## 2026-05-19 - Avoid std::ostringstream inside formatting loops
**Learning:** `std::ostringstream` exhibits significant overhead for string concatenation and formatting (like hex dumps or unicode character escaping) due to virtual function calls, locale initialization, and dynamic memory allocations. In tight loops (like `calculateSHA256` or `jsonEscape`), repeatedly creating an `ostringstream` to format integers or characters creates a substantial CPU and memory bottleneck, dropping throughput by up to 25%.
**Action:** Replace `std::ostringstream` with a pre-sized `std::string` (`.reserve()` or `.resize()`), and format primitive types directly into fixed-size stack buffers (e.g. `char buf[64]`) using `snprintf`, then append them to the target string.
## 2026-06-30 - Maintain exact float formatting parity when optimizing string concatenation
**Learning:** When replacing `std::ostringstream` with pre-allocated `std::string` concatenation in hot paths, using `std::to_string(double)` introduces a formatting regression (fixed 6 decimal places, e.g. "1.000000") which breaks backwards compatibility with previous JSON outputs.
**Action:** Use `snprintf` with a properly-sized stack buffer (e.g., `char buf[512]`) and the `%g` format specifier to accurately replicate the original `ostringstream` floating-point formatting, avoiding both the formatting regression and dynamic allocations.
## 2026-06-30 - Avoid redundant cv::Mat::clone() before ResultPublisher::publish()
**Learning:** In the C++ rendering pipeline (`Engine::renderResults()`), calling `cv::Mat::clone()` before passing frames to `ResultPublisher::publish()` is a redundant deep copy. The publisher inherently uses `copyTo()` to safely duplicate the data into its internal buffer, making the upstream deep copy unnecessary and costly in terms of per-frame heap allocations and memory bandwidth.
**Action:** Remove `clone()` calls when passing read-only `cv::Mat` frames to `ResultPublisher::publish()` unless the frame is actively modified (e.g., via `cv::putText`) prior to publishing, in which case `clone()` MUST be preserved to prevent corrupting the underlying source buffer.

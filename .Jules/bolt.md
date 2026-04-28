
## 2026-04-03 - Optimization with std::partial_sort
**Learning:** Using std::stable_sort on an entire large vector (e.g., in FaceSearch::searchTopK) when only topK elements are needed is an O(N log N) bottleneck.
**Action:** Use std::partial_sort for O(N log K) performance when returning Top-K results. For stability, incorporate the element's original index into the comparison function.
## 2024-04-06 - Avoid committing compiled benchmark binaries
**Learning:** Writing custom benchmark scripts is a good way to test C++ performance on loops, but adding them to version control pollutes the repository and slows things down.
**Action:** Do not commit temporary testing files, benchmark scripts, or binaries used for local performance validation to the repository's version history.
## 2026-04-05 - Use float for embedding distance metrics
**Learning:** Using `double` for accumulation inside tight loops (like `l2Norm` and `cosineSimilarity` in FaceSearch.cpp) on arrays of `float` creates an unnecessary performance bottleneck due to continuous type promotion and reduced SIMD utilization, especially on ARM Cortex-A17.
**Action:** Always use `float` accumulation variables when processing `float` arrays in performance-critical loops unless exact double-precision is mathematically required.
## 2024-05-24 - Vector L2 Normalization Loop Optimization
**Learning:** Using `double` for accumulation inside tight loops (like `l2NormalizeInplace` in ArcFaceEmbedder.cpp and FaceInferencePipeline.cpp) on arrays of `float` creates an unnecessary performance bottleneck due to continuous type promotion and reduced SIMD utilization, especially on ARM Cortex-A17.
**Action:** Always use `float` accumulation variables when processing `float` arrays in performance-critical loops unless exact double-precision is mathematically required.

## 2024-05-14 - Optimize TopK Face Search string copying
**Learning:** During face search, creating `FaceSearchHit` for every item in the dataset involves copying string IDs (which can be long). This is inefficient when only `topK` items are needed and causes unnecessary string allocations and copies in the hot loop.
**Action:** Use a lightweight `FastHit` struct containing only `index` and `score` during the distance calculation and sorting loop. Map the sorted indices back to `FaceSearchHit` items (and their string IDs) only for the `topK` items after sorting is complete.

## 2026-04-12 - Benchmark Pure Inference Timing Boundary
**Learning:** Including model input setup functions (like `net.setInput` or `ex.input`) and extractor creation inside the performance timing boundary introduces overhead and noise (jitter). This inflates measurements intended to capture purely the forward-pass or execution time of the network.
**Action:** When measuring model inference latency, strictly bind the timer immediately before the actual forward execution (e.g., `net.forward()` or `ex.extract()`) and immediately after, excluding any state initialization or memory copying overhead from the measurement.

## 2026-04-17 - Avoid cv::cvtColor for NCNN Pixel Conversion
**Learning:** Using `cv::cvtColor` to swap color channels (e.g., BGR to RGB) before passing a `cv::Mat` to NCNN introduces redundant memory allocation and copy overhead per frame. This is a common bottleneck in tight inference loops.
**Action:** Use NCNN's native pixel conversion flags (e.g., `ncnn::Mat::PIXEL_BGR2RGB`) within `ncnn::Mat::from_pixels()` directly. Ensure the input `cv::Mat` is continuous (`isContinuous()`) before conversion to avoid access violations.
## 2025-01-20 - YUV420 to I420 Conversion Pointer Optimization
**Learning:** Double loops with indexing and multiplications (`col * yPixelStride`) inside tight inner loops for YUV420 pixel extraction generate redundant instructions and prevent compiler vectorization, slowing down end-to-end frame decoding times.
**Action:** Use sequential pointer arithmetic to traverse image rows and pixels, moving the pointer directly rather than recalculating the memory offset on every iteration.

## 2024-05-18 - 优化帧处理与渲染中的内存分配
**Learning:** `cv::Mat::clone()` 强制执行深度内存拷贝并分配全新内存块。如果在热门的处理循环（如 `Engine::processFrame`、`FramePipeline::processLoop`）中频繁使用，会导致持续的堆内存分配、较高的内存碎片率以及潜在的 GC 抖动（在跨 JNI 或大量小对象分配时尤为明显）。
**Action:** 使用预先分配的/持久化的缓冲矩阵（如类成员或被置于循环外部的 `drawBuffer`/`frameBuffer`），并配合 `cv::Mat::copyTo()`，从而仅执行数据覆盖而无需重新分配，这在保证同样线程安全性的同时显著降低了动态分配开销。


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
## 2025-04-18 - Avoid redundant cv::cvtColor allocation in ncnn inference
**Learning:** cv::cvtColor creates a new cv::Mat with new memory allocations in tight inference loop in ArcFaceEmbedder.cpp. This causes performance bottleneck and memory overhead.
**Action:** Use NCNN's native pixel conversion flags (e.g., `ncnn::Mat::PIXEL_BGR2RGB`) within `ncnn::Mat::from_pixels()` to swap color channels without cv::cvtColor. Ensure the `meanVals` channel order matches the input pixel format (e.g., R,G,B for PIXEL_BGR2RGB), and verify the source `cv::Mat` is continuous outside the loop (`!bgr.isContinuous() -> bgr = bgr.clone()`).

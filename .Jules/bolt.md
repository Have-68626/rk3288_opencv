
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

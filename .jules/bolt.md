
## 2026-04-03 - Optimization with std::partial_sort
**Learning:** Using std::stable_sort on an entire large vector (e.g., in FaceSearch::searchTopK) when only topK elements are needed is an O(N log N) bottleneck.
**Action:** Use std::partial_sort for O(N log K) performance when returning Top-K results. For stability, incorporate the element's original index into the comparison function.
## 2024-04-06 - Avoid committing compiled benchmark binaries
**Learning:** Writing custom benchmark scripts is a good way to test C++ performance on loops, but adding them to version control pollutes the repository and slows things down.
**Action:** Do not commit temporary testing files, benchmark scripts, or binaries used for local performance validation to the repository's version history.
## 2026-04-05 - Use float for embedding distance metrics
**Learning:** Using `double` for accumulation inside tight loops (like `l2Norm` and `cosineSimilarity` in FaceSearch.cpp) on arrays of `float` creates an unnecessary performance bottleneck due to continuous type promotion and reduced SIMD utilization, especially on ARM Cortex-A17.
**Action:** Always use `float` accumulation variables when processing `float` arrays in performance-critical loops unless exact double-precision is mathematically required.

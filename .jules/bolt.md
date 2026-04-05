
## 2026-04-03 - Optimization with std::partial_sort
**Learning:** Using std::stable_sort on an entire large vector (e.g., in FaceSearch::searchTopK) when only topK elements are needed is an O(N log N) bottleneck.
**Action:** Use std::partial_sort for O(N log K) performance when returning Top-K results. For stability, incorporate the element's original index into the comparison function.

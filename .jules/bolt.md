## 2026-04-03 - Optimization with std::partial_sort
**Learning:** Using std::stable_sort on an entire large vector (e.g., in FaceSearch::searchTopK) when only topK elements are needed is an O(N log N) bottleneck.
**Action:** Use std::partial_sort for O(N log K) performance when returning Top-K results. For stability, incorporate the element's original index into the comparison function.

## 2025-04-04 - Optimize FaceSearch linear index Top-K sorting
**Learning:** For a simple linear index scan, Top-K retrieval time was dominated by `std::stable_sort` over the full dataset (O(N log N)), only to discard everything but the top K items. Because the index uses a deterministic unique index to break ties, stable sort isn't needed—regular sort functions perfectly.
**Action:** Always prefer partial sorts (O(N log K)) like `std::partial_sort` or `std::nth_element` + `std::sort` when a function requires returning only the Top-K elements from a large result set.

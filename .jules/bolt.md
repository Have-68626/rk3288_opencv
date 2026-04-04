## 2025-04-04 - [Optimize FaceSearch linear index Top-K sorting]
**Learning:** For a simple linear index scan, Top-K retrieval time was dominated by `std::stable_sort` over the full dataset ($O(N \log N)$), only to discard everything but the top K items. Because the index uses a deterministic unique index to break ties, stable sort isn't needed—regular sort functions perfectly.
**Action:** Always prefer partial sorts ($O(N \log K)$) like `std::partial_sort` or `std::nth_element` + `std::sort` when a function requires returning only the Top-K elements from a large result set.

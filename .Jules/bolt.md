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

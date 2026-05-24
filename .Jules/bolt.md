## 2024-05-22 - Replacing ostringstream with string concatenation for JSON formatting

**Learning:** When replacing `std::ostringstream` with pre-allocated `std::string` concatenation for JSON formatting, `std::to_string(double)` produces a fixed 6 decimal places (e.g., `1.000000`), breaking exact string formatting parity with `ostringstream`.
**Action:** Use `snprintf` with `%g` to accurately replicate the original `ostringstream` floating-point formatting.

## 2025-02-14 - FFmpegKit Command Injection via Reflection Call

**Vulnerability:** Command injection due to unescaped string concatenation when building an FFmpeg command that is passed to a dynamically loaded FFmpegKit module via reflection (`kit.getMethod("executeAsync", String.class)`).

**Learning:** When dealing with legacy or dynamically loaded libraries where you must use a string-based command signature (e.g. `executeAsync(String)` instead of an array-based one), relying on simple quoting (e.g. replacing `"` with `\"` but not enclosing the string safely or ignoring the spaces parser) is insufficient. FFmpegKit internally tokenizes string commands using spaces and single/double quotes, which allows an attacker to break out of weakly escaped arguments.

**Prevention:** To safely build a string command for tokenizing parsers, collect arguments in a list, strictly escape internal single quotes (`'` -> `'\''`), completely wrap each individual argument in single quotes (`'...'`), and only then join the arguments with a space. Avoid modifying reflection signatures (e.g. switching to `String[]`) if backward compatibility with unknown external/legacy AARs is a hard requirement.

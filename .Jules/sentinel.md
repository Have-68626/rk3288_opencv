# Sentinel Security Memory

## Data Protection
- Redact sensitive credentials before they are written to disk, not only when logs are exported or displayed.
- Keep redaction rules centralized in `SensitiveDataUtil.maskSensitiveData` so ingestion, UI display, and export follow the same masking policy.
- Do not trust file extensions to decide whether log content is safe to export. Use content-based text detection and skip undecodable binary content instead of streaming raw bytes.

## Network And Service Hardening
- The local HTTP server uses a thread-per-connection model, so accepted client sockets must receive explicit read and write timeouts immediately after `accept()`.
- Keep local-scope services conservative by default and avoid broadening exposure without an explicit requirement.
- Public-facing security summaries should avoid exploit-ready detail and focus on the fix, impact, verification, and rollback path.

## Path And Input Validation
- For Java file handling, validate canonical paths with `canonicalPath.startsWith(allowedDir.getCanonicalPath() + File.separator)` before allowing file access.
- Custom path decoders and parsers must fail closed on truncated or invalid escape sequences, especially percent-encoding at string boundaries.
- Treat user-controlled paths, URL components, and exported filenames as hostile input until validated.

## Security Review Bias
- Prefer small, reviewable fixes that remove leak paths, path traversal, unsafe parser behavior, and denial-of-service edges.
- Do not weaken validation or logging safeguards just to make a test or local smoke check pass.
## 2025-02-14 - FFmpegKit Command Injection via Reflection Call

**Vulnerability:** Command injection due to unescaped string concatenation when building an FFmpeg command that is passed to a dynamically loaded FFmpegKit module via reflection (`kit.getMethod("executeAsync", String.class)`).

**Learning:** When dealing with legacy or dynamically loaded libraries where you must use a string-based command signature (e.g. `executeAsync(String)` instead of an array-based one), relying on simple quoting (e.g. replacing `"` with `\"` but not enclosing the string safely or ignoring the spaces parser) is insufficient. FFmpegKit internally tokenizes string commands using spaces and single/double quotes, which allows an attacker to break out of weakly escaped arguments.

**Prevention:** To safely build a string command for tokenizing parsers, collect arguments in a list, strictly escape internal single quotes (`'` -> `'\''`), completely wrap each individual argument in single quotes (`'...'`), and only then join the arguments with a space. Avoid modifying reflection signatures (e.g. switching to `String[]`) if backward compatibility with unknown external/legacy AARs is a hard requirement.
## 2026-05-18 - Enforce global redaction of sensitive credentials at rest
**Vulnerability:** Passwords, tokens, and authorization keys were only redacted during log export (in `LogViewerActivity.java`) but were written to disk in plain text by `AppLog.java`, risking credential exposure if local logs were compromised.
**Learning:** Redaction rules defined in UI/export layers often miss the primary persistent storage layer.
**Prevention:** Centralize all sensitive data masking rules (e.g., in `SensitiveDataUtil.java`) and apply them at the point of ingestion/logging before data hits the disk.
## 2026-05-18 - Fix FFmpeg command injection vulnerability via argument list parsing
**Vulnerability:** `FfmpegRtmpPusher` dynamically constructed commands via simple string concatenation and double-quote escaping. This naive escaping (merely escaping double quotes) failed to handle backslashes or shell metacharacters, allowing arbitrary arguments to be injected.
**Learning:** In Java when integrating with frameworks that parse strings into shell-like arguments (like `FFmpegKit`), manual string concatenation with naive escaping is extremely error-prone and easily broken by inputs containing spaces, quotes, or backslashes.
**Prevention:** Always collect command arguments into a `List<String>`. Prefer using array-based APIs (e.g., `executeAsync(String[])`) to avoid manual quoting and parsing issues. If a single string is strictly required by a legacy API, use robust shell-style quoting (e.g., single quotes with internal single quotes escaped as `'\''`).

## 2026-05-20 - Limit HTTP Server Concurrent Connections
**Vulnerability:** The local HTTP server `HttpFacesServer.cpp` blindly accepted connections and spawned a detached thread for every incoming socket (`std::thread([this, cs]() { handleClient... }).detach()`) without enforcing an upper limit on concurrent connections. A local malicious or misbehaving client could rapidly open thousands of connections, leading to uncontrolled thread spawning, resource exhaustion (Thread Exhaustion DoS), and ultimately crashing the application.
**Learning:** Even loopback-bound services (like this one on 127.0.0.1) can suffer from denial of service if connection and thread lifetimes aren't bounded. Unbounded thread allocation per connection is dangerous.
**Prevention:** Track concurrent connections using an atomic counter (`activeConnections_`) and enforce a hard limit (e.g., 64) in the accept loop. Immediately reject connections (via `closesocket()`) if the threshold is reached before spawning a thread. Ensure the counter accurately decrements when the connection handler terminates.
## 2026-05-20 - Fix Flawed DOS Accounting in Local HTTP Server
**Vulnerability:** While an atomic connection guard existed, a logical flaw caused `activeConnections_` to be incremented twice per connection and decremented in uncoordinated contexts (the detached thread lambda and `handleClient`'s `ConnectionGuard`). This would eventually cause the counter to wrap negative and permanently break the concurrent connection cap.
**Learning:** Concurrent resource accounting must be strictly balanced. Mixing RAII guards (`ConnectionGuard`) with manual ad-hoc increments/decrements in thread dispatch blocks creates complex desynchronization bugs.
**Prevention:** Rely strictly on scoped RAII guards for decrementing connection counts inside thread lifetimes, and ensure exactly one matching increment exists before the thread spawns.
## 2026-06-01 - Universally mask sensitive data across all log levels before writing to disk
**Vulnerability:** Although sensitive credentials were being masked for INFO/WARN/ERROR/FATAL log levels during disk writing, DEBUG and VERBOSE level logs bypassed the redaction logic in `AppLog.java`. This allowed raw, unmasked sensitive data to be written in plaintext to the local logs when the log level was set to DEBUG/VERBOSE, creating a potential credential exposure if local logs were compromised.
**Learning:** Masking and redaction mechanisms must be applied universally across all log levels before persistence. Assuming DEBUG/VERBOSE environments are safe for plaintext credentials is a flawed policy when those logs are persistently stored on the device.
**Prevention:** Apply data redaction (e.g., `SensitiveDataUtil.maskSensitiveData`) to all log levels uniformly before writing to disk. To mitigate any CPU overhead from universal masking, implement a low-cost string pre-check to bypass heavy regex operations for safe strings that don't contain common sensitive keywords or digits.

## 2025-02-14 - FFmpegKit Command Injection via Reflection Call

**Vulnerability:** Command injection due to unescaped string concatenation when building an FFmpeg command that is passed to a dynamically loaded FFmpegKit module via reflection (`kit.getMethod("executeAsync", String.class)`).

**Learning:** When dealing with legacy or dynamically loaded libraries where you must use a string-based command signature (e.g. `executeAsync(String)` instead of an array-based one), relying on simple quoting (e.g. replacing `"` with `\"` but not enclosing the string safely or ignoring the spaces parser) is insufficient. FFmpegKit internally tokenizes string commands using spaces and single/double quotes, which allows an attacker to break out of weakly escaped arguments.

**Prevention:** To safely build a string command for tokenizing parsers, collect arguments in a list, strictly escape internal single quotes (`'` -> `'\''`), completely wrap each individual argument in single quotes (`'...'`), and only then join the arguments with a space. Avoid modifying reflection signatures (e.g. switching to `String[]`) if backward compatibility with unknown external/legacy AARs is a hard requirement.
## 2026-05-18 - Enforce global redaction of sensitive credentials at rest
**Vulnerability:** Passwords, tokens, and authorization keys were only redacted during log export (in `LogViewerActivity.java`) but were written to disk in plain text by `AppLog.java`, risking credential exposure if local logs were compromised.
**Learning:** Redaction rules defined in UI/export layers often miss the primary persistent storage layer.
**Prevention:** Centralize all sensitive data masking rules (e.g., in `SensitiveDataUtil.java`) and apply them at the point of ingestion/logging before data hits the disk.

## 2026-05-20 - Limit HTTP Server Concurrent Connections
**Vulnerability:** The local HTTP server `HttpFacesServer.cpp` blindly accepted connections and spawned a detached thread for every incoming socket (`std::thread([this, cs]() { handleClient... }).detach()`) without enforcing an upper limit on concurrent connections. A local malicious or misbehaving client could rapidly open thousands of connections, leading to uncontrolled thread spawning, resource exhaustion (Thread Exhaustion DoS), and ultimately crashing the application.
**Learning:** Even loopback-bound services (like this one on 127.0.0.1) can suffer from denial of service if connection and thread lifetimes aren't bounded. Unbounded thread allocation per connection is dangerous.
**Prevention:** Track concurrent connections using an atomic counter (`activeConnections_`) and enforce a hard limit (e.g., 64) in the accept loop. Immediately reject connections (via `closesocket()`) if the threshold is reached before spawning a thread. Ensure the counter accurately decrements when the connection handler terminates.

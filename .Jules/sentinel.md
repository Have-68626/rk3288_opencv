## 2025-04-12 - Fix sensitive data leak in log export
**Vulnerability:** The LogViewerActivity's `performExport` method directly read log files using a `FileInputStream` and wrote their raw bytes to a zip archive for any file that didn't have a `.log` or `.txt` extension. This bypassed the redaction logic, potentially leaking sensitive information (tokens, passwords, emails, phone numbers) in plain text.
**Learning:** Fallback to raw byte streaming for files without specific extensions or ambiguous content types is dangerous when the primary path requires privacy/security redaction. An attacker or unexpected condition (e.g. rotated log file without `.log` extension) can bypass the redaction.
**Prevention:** Do not rely on file extensions to determine if a file should be treated as text for redaction. Instead, securely check if the file is valid UTF-8 using a `CharsetDecoder` with `CodingErrorAction.REPORT`. If it is text, redact and export. If it is binary/non-decodable, skip it and log the skip to prevent silent leaks.

## 2026-04-14 - Fix Local HTTP Server Slowloris DoS
**Vulnerability:** The local HTTP server `HttpFacesServer.cpp` accepted connections using blocking sockets but failed to configure any read (`SO_RCVTIMEO`) or write (`SO_SNDTIMEO`) timeouts. Since each connection was handled in a newly spawned detached thread, a malicious client could open many connections and intentionally send data extremely slowly, causing thread exhaustion and application crash.
**Learning:** Default blocking sockets on Windows/Linux without timeout configurations are highly susceptible to resource exhaustion attacks when paired with a thread-per-connection model.
**Prevention:** Always set explicit receive and send timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`) on client sockets immediately after `accept()`.

## 2026-04-15 - Enforce Strict URL Decoding Boundary Validation
**Vulnerability:** URL percent-encoding validation could be bypassed using incomplete sequences (like a lone `%`) at the end of the input string because `urlDecodePath` incorrectly treated them as literal characters instead of triggering a validation failure. This could potentially disrupt WAF logic or downstream canonicalization checks.
**Learning:** Custom decoding loops often fail to handle edge cases at the very end of string boundaries, falling through to default literal assignment instead of safely aborting.
**Prevention:** When implementing or reviewing custom parsers (e.g., URL decoding), ensure boundary edge cases correctly return an explicit failure state rather than silently accepting truncated input.

## 2026-05-18 - Enforce global redaction of sensitive credentials at rest
**Vulnerability:** Passwords, tokens, and authorization keys were only redacted during log export (in `LogViewerActivity.java`) but were written to disk in plain text by `AppLog.java`, risking credential exposure if local logs were compromised.
**Learning:** Redaction rules defined in UI/export layers often miss the primary persistent storage layer.
**Prevention:** Centralize all sensitive data masking rules (e.g., in `SensitiveDataUtil.java`) and apply them at the point of ingestion/logging before data hits the disk.

## 2026-05-20 - Limit HTTP Server Concurrent Connections
**Vulnerability:** The local HTTP server `HttpFacesServer.cpp` blindly accepted connections and spawned a detached thread for every incoming socket (`std::thread([this, cs]() { handleClient... }).detach()`) without enforcing an upper limit on concurrent connections. A local malicious or misbehaving client could rapidly open thousands of connections, leading to uncontrolled thread spawning, resource exhaustion (Thread Exhaustion DoS), and ultimately crashing the application.
**Learning:** Even loopback-bound services (like this one on 127.0.0.1) can suffer from denial of service if connection and thread lifetimes aren't bounded. Unbounded thread allocation per connection is dangerous.
**Prevention:** Track concurrent connections using an atomic counter (`activeConnections_`) and enforce a hard limit (e.g., 64) in the accept loop. Immediately reject connections (via `closesocket()`) if the threshold is reached before spawning a thread. Ensure the counter accurately decrements when the connection handler terminates.

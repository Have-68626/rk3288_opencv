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

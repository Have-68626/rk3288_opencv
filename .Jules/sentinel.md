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

## 2024-10-24 - Missing Security Headers in Local API
**Vulnerability:** The local C++ HTTP server (`HttpFacesServer.cpp`) lacked standard security headers (`X-Content-Type-Options`, `X-Frame-Options`, `Content-Security-Policy`).
**Learning:** Local APIs often skip basic HTTP security configurations, mistakenly relying on their "localhost" binding. This can leave them exposed to Cross-Site Scripting (XSS), clickjacking, and MIME sniffing attacks if a malicious site executed in a browser interacts with them locally.
**Prevention:** Always enforce a baseline of security headers (`X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Content-Security-Policy: default-src 'none'`) on all HTTP responses, regardless of whether the server is public-facing or restricted to `localhost`.
## 2024-05-24 - Cryptographic Exception Swallowing
**Vulnerability:** `FeatureTemplateEncryptedStore` was swallowing `NoSuchAlgorithmException` / `NoSuchPaddingException` when initializing standard cryptographic algorithms like AES-GCM, wrapping them in generic `Exception` blocks.
**Learning:** This masks security-critical initialization failures as standard errors, potentially resulting in generic error codes (like `IO_ERROR` instead of `KEYSTORE_ERROR`) that obscure platform security incompatibilities.
**Prevention:** Never catch generic `Exception` for cryptographic initializers. Instead, use specific catch blocks for `NoSuchAlgorithmException` etc. and wrap them in an explicit `IllegalStateException` mapped to security-specific error codes.

## 2024-05-24 - Cryptographic Exception Swallowing
**Vulnerability:** `FeatureTemplateEncryptedStore` was swallowing `NoSuchAlgorithmException` / `NoSuchPaddingException` when initializing standard cryptographic algorithms like AES-GCM, wrapping them in generic `Exception` blocks.
**Learning:** This masks security-critical initialization failures as standard errors, potentially resulting in generic error codes (like `IO_ERROR` instead of `KEYSTORE_ERROR`) that obscure platform security incompatibilities.
**Prevention:** Never catch generic `Exception` for cryptographic initializers. Instead, use specific catch blocks for `NoSuchAlgorithmException` etc. and wrap them in an explicit `IllegalStateException` mapped to security-specific error codes.

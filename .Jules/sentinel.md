## 2024-05-22 - Default permissive CORS configuration in CivetWeb
**Vulnerability:** The embedded CivetWeb server was configured with `*` as the default value for `access_control_allow_origin`, `access_control_allow_methods`, and `access_control_allow_headers`.
**Learning:** This exposes the application to Cross-Origin Resource Sharing attacks out-of-the-box, allowing arbitrary websites to make cross-origin requests to the local server. The frontend and backend share the exact same origin, making CORS entirely unnecessary.
**Prevention:** Always default CORS configurations to completely disabled (`NULL` in CivetWeb to omit the headers completely) unless specifically required, and enforce a strict whitelist if CORS is needed.

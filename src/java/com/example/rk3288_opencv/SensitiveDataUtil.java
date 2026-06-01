package com.example.rk3288_opencv;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class SensitiveDataUtil {

    // Regex patterns for sensitive data
    private static final Pattern PHONE_PATTERN = Pattern.compile("(?<!\\d)(1[3-9]\\d{9})(?!\\d)");
    private static final Pattern ID_CARD_PATTERN = Pattern.compile("(?<!\\d)(\\d{6})(19|20)(\\d{2})(0[1-9]|1[0-2])(0[1-9]|[12]\\d|3[01])(\\d{3}[0-9Xx])(?!\\d)");
    // Simple GPS pattern (Lat, Lon) e.g., "39.9042, 116.4074"
    private static final Pattern GPS_PATTERN = Pattern.compile("(-?\\d{1,3}\\.\\d{4,}),\\s*(-?\\d{1,3}\\.\\d{4,})");

    // Authentication credentials and tokens
    private static final Pattern SENSITIVE_KV = Pattern.compile(
            "(?i)\\b(password|passwd|pwd|token|access[_-]?token|refresh[_-]?token|authorization|bearer|secret|api[_-]?key)\\b(\\s*[:=]\\s*)([^\\s,;]+)"
    );
    private static final Pattern EMAIL_PATTERN = Pattern.compile("(?i)\\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}\\b");

    /**
     * Masks sensitive data in the given text.
     * @param text Original text
     * @return Text with sensitive data replaced by ***
     */
    public static String maskSensitiveData(String text) {
        if (text == null || text.isEmpty()) return text;

        // Low-cost pre-check to avoid heavy regex on safe strings
        String lower = text.toLowerCase();
        boolean hasDigit = false;
        boolean hasKeyword = lower.contains("pass") || lower.contains("token") ||
                lower.contains("auth") || lower.contains("key") ||
                lower.contains("secret") || lower.contains("@");

        for (int i = 0; i < text.length(); i++) {
            if (Character.isDigit(text.charAt(i))) {
                hasDigit = true;
                break;
            }
        }

        if (!hasKeyword && !hasDigit) {
            return text;
        }

        String result = text;

        // Mask Phone Numbers: 13812345678 -> 138****5678
        Matcher phoneMatcher = PHONE_PATTERN.matcher(result);
        StringBuffer phoneSb = new StringBuffer();
        while (phoneMatcher.find()) {
            String phone = phoneMatcher.group(1);
            String masked = phone.substring(0, 3) + "****" + phone.substring(7);
            phoneMatcher.appendReplacement(phoneSb, masked);
        }
        phoneMatcher.appendTail(phoneSb);
        result = phoneSb.toString();

        // Mask ID Cards: 110101199003071234 -> 110101********1234
        Matcher idMatcher = ID_CARD_PATTERN.matcher(result);
        StringBuffer idSb = new StringBuffer();
        while (idMatcher.find()) {
            String id = idMatcher.group(0); // Full match
            String masked = id.substring(0, 6) + "********" + id.substring(14);
            idMatcher.appendReplacement(idSb, masked);
        }
        idMatcher.appendTail(idSb);
        result = idSb.toString();
        
        // Mask GPS: 39.9042, 116.4074 -> ***, ***
        Matcher gpsMatcher = GPS_PATTERN.matcher(result);
        result = gpsMatcher.replaceAll("***, ***");

        /*
         * 威胁模型：避免运行时的凭据（Token/Password/Secret）以及邮箱通过 AppLog 以明文落盘，导致设备被 root 或日志被非授权读取时泄露高价值凭据。
         * 为什么这样改：原逻辑仅在日志导出时 (LogViewerActivity) 对凭据进行脱敏，但落盘日志依然是明文。将其统一移至 SensitiveDataUtil，使全局落盘和展示都生效。
         * 影响范围：所有调用 SensitiveDataUtil.maskSensitiveData 的日志记录、UI 显示和导出均会应用凭据与邮箱掩码。
         * 回滚方式：删除 SENSITIVE_KV 和 EMAIL_PATTERN 的正则替换逻辑即可。
         */
        Matcher kvMatcher = SENSITIVE_KV.matcher(result);
        StringBuffer kvSb = new StringBuffer();
        while (kvMatcher.find()) {
            String k = kvMatcher.group(1);
            String sep = kvMatcher.group(2); // Assuming regex was updated to capture separator
            String v = kvMatcher.group(3);
            String masked = "***";
            int len = v.length();
            if (len > 4) {
                int keep = Math.min(2, len);
                masked = v.substring(0, keep) + "***" + v.substring(len - keep);
            }
            kvMatcher.appendReplacement(kvSb, Matcher.quoteReplacement(k + sep + masked));
        }
        kvMatcher.appendTail(kvSb);
        result = kvSb.toString();

        result = EMAIL_PATTERN.matcher(result).replaceAll("***@***");

        return result;
    }
}

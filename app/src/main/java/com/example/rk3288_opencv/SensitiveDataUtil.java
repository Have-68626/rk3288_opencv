package com.example.rk3288_opencv;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class SensitiveDataUtil {

    // Regex patterns for sensitive data
    private static final Pattern PHONE_PATTERN = Pattern.compile("(?<!\\d)(1[3-9]\\d{9})(?!\\d)");
    private static final Pattern ID_CARD_PATTERN = Pattern.compile("(?<!\\d)(\\d{6})(19|20)(\\d{2})(0[1-9]|1[0-2])(0[1-9]|[12]\\d|3[01])(\\d{3}[0-9Xx])(?!\\d)");
    // Simple GPS pattern (Lat, Lon) e.g., "39.9042, 116.4074"
    private static final Pattern GPS_PATTERN = Pattern.compile("(-?\\d{1,3}\\.\\d{4,}),\\s*(-?\\d{1,3}\\.\\d{4,})");

    /**
     * Masks sensitive data in the given text.
     * @param text Original text
     * @return Text with sensitive data replaced by ***
     */
    public static String maskSensitiveData(String text) {
        if (text == null || text.isEmpty()) return text;

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

        return result;
    }
}

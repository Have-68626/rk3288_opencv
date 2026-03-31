package com.example.rk3288_opencv;

import androidx.annotation.NonNull;

import java.util.Locale;

final class DeviceClassifier {
    private DeviceClassifier() {
    }

    @NonNull
    static DeviceClass classify(@NonNull DeviceProfile p) {
        if (isIndustrialRk3288(p)) return DeviceClass.INDUSTRIAL_RK3288;

        String m = upper(p.manufacturer);
        String b = upper(p.brand);

        if (containsAny(m, b, "OPPO", "REALME", "ONEPLUS")) return DeviceClass.CN_OPPO;
        if (containsAny(m, b, "VIVO", "IQOO")) return DeviceClass.CN_VIVO;
        if (containsAny(m, b, "XIAOMI", "REDMI", "POCO")) return DeviceClass.CN_XIAOMI;
        if (containsAny(m, b, "HUAWEI", "HONOR")) return DeviceClass.CN_HUAWEI;

        if (p.hasGms && !isLikelyMainlandDevice(p)) return DeviceClass.GLOBAL_GMS;
        return DeviceClass.OTHER;
    }

    private static boolean isIndustrialRk3288(@NonNull DeviceProfile p) {
        String hw = upper(p.hardware);
        String board = upper(p.board);
        boolean isRk3288 = hw.contains("RK3288") || board.contains("RK3288");
        boolean isAndroid7 = (p.sdkInt == 24 || p.sdkInt == 25);
        boolean isUserDebug = "USERDEBUG".equalsIgnoreCase(p.buildType);
        boolean mem2g = approxInRange(p.totalMemBytes, 2L * 1024L * 1024L * 1024L, 0.35);
        boolean rom8g = approxInRange(p.dataTotalBytes, 8L * 1024L * 1024L * 1024L, 0.35);
        return isRk3288 && isAndroid7 && isUserDebug && mem2g && rom8g;
    }

    private static boolean isLikelyMainlandDevice(@NonNull DeviceProfile p) {
        String f = upper(p.fingerprint);
        String m = upper(p.manufacturer);
        if (containsAny(m, m, "OPPO", "VIVO", "XIAOMI", "REDMI", "HUAWEI", "HONOR")) return true;
        return f.contains("CHINA") || f.contains("CN");
    }

    private static boolean approxInRange(long value, long expected, double ratio) {
        if (value <= 0 || expected <= 0) return false;
        double lo = expected * (1.0 - ratio);
        double hi = expected * (1.0 + ratio);
        return value >= lo && value <= hi;
    }

    private static boolean containsAny(@NonNull String a, @NonNull String b, @NonNull String... needles) {
        for (String n : needles) {
            String nn = upper(n);
            if (a.contains(nn) || b.contains(nn)) return true;
        }
        return false;
    }

    @NonNull
    private static String upper(@NonNull String s) {
        return s.toUpperCase(Locale.US);
    }
}


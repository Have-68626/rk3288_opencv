package com.example.rk3288_opencv;

import androidx.annotation.NonNull;

import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

/**
 * 历史明文显示名的**检测与清除策略**（任务 B，2026-09-16 决策）。
 *
 * <p><b>为什么保留而不自动删除</b>：API 21–22 上本项目**尚未实现并验证**适用于该 API 级别的替代
 * 加密方案（注意：Android Keystore provider 自 API 18 即存在，缺少 Keystore AES 不等于完全没有
 * 密钥保护；Keystore 中的 RSA 包裹随机 AES 密钥是一条尚未落地的候选路径）。因此本批**不启用**
 * API&lt;23 的显示名加密持久化，也**不回落到新增明文**；对**历史**明文键采取**默认保留 + 用户确认后
 * 清除**：不在启动、升级或迁移失败时自动删除——保留是**避免未经同意丢失数据**，不是低估 PII 风险。
 *
 * <p>⚠️ **不得**因此宣称「设备上已无明文 PII」；只要历史键存在，明文就在。
 *
 * <p><b>为什么策略层与 UI 分离</b>：纯集合运算可被 JVM 单测覆盖（本仓无 Robolectric，无法构造
 * {@code MainActivity}）；这里只决定「选中哪些键」「是否存在需提示的历史名」，实际的持久化删除
 * 由调用方用 {@code SharedPreferences.Editor.commit()} 完成并检查返回值。
 */
final class LegacyDisplayNames {

    /** 【历史】任务18 的 personId → 显示名明文映射前缀；任务 B 起不再写入，仅用于检测与清除。 */
    static final String PREF_PREFIX = "pref_person_name_";

    private LegacyDisplayNames() {
    }

    /**
     * 选出**待清除**的历史键：仅「以 {@link #PREF_PREFIX} 开头」且「值为 String」的键。
     *
     * <p>**绝不**匹配其他设置键——`SharedPreferences` 里还有 10 余个无关配置（浮层、节流、上次
     * 预检结果等），清除流程一旦误伤就是用户配置丢失。
     *
     * <p>空串值也会被选中：旧实现连空显示名都写入（`putString(prefix+id, "")`），属纯残留，随之清掉。
     */
    @NonNull
    static Set<String> selectKeysForClearing(@NonNull Map<String, ?> all) {
        Set<String> keys = new LinkedHashSet<>();
        for (Map.Entry<String, ?> e : all.entrySet()) {
            String k = e.getKey();
            if (k == null || !k.startsWith(PREF_PREFIX)) continue;
            if (!(e.getValue() instanceof String)) continue;
            keys.add(k);
        }
        return keys;
    }

    /**
     * 是否存在**仍保留显示名**的历史键（值为非空 String）——用于决定是否向用户提示。
     *
     * <p>只含空串的残留不算「保留了显示名」：它不含 PII，不值得打扰用户（清除流程仍会一并清掉它）。
     */
    static boolean hasRetainedNames(@NonNull Map<String, ?> all) {
        for (Map.Entry<String, ?> e : all.entrySet()) {
            String k = e.getKey();
            if (k == null || !k.startsWith(PREF_PREFIX)) continue;
            Object v = e.getValue();
            if (v instanceof String && !((String) v).trim().isEmpty()) return true;
        }
        return false;
    }

    /** 待清除键的数量（提示文案用）。 */
    static int retainedCount(@NonNull Map<String, ?> all) {
        int n = 0;
        for (Map.Entry<String, ?> e : all.entrySet()) {
            String k = e.getKey();
            if (k == null || !k.startsWith(PREF_PREFIX)) continue;
            Object v = e.getValue();
            if (v instanceof String && !((String) v).trim().isEmpty()) n++;
        }
        return n;
    }
}

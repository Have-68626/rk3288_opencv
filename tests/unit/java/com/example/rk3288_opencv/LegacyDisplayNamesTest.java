package com.example.rk3288_opencv;

import org.junit.Test;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * 历史明文显示名的检测与清除策略（任务 B 决策：默认保留、用户确认后清除）。
 *
 * <p>纯集合运算，可在 JVM 直接跑——这正是把策略从 {@code MainActivity} 里分出来的原因
 *（本仓无 Robolectric/Mockito，`MainActivity` 无法在纯 JVM 构造）。
 */
public class LegacyDisplayNamesTest {

    /** 与真实 SharedPreferences 同构的输入：显示名键 + 若干无关设置键。 */
    private static Map<String, Object> prefs() {
        Map<String, Object> m = new LinkedHashMap<>();
        m.put(LegacyDisplayNames.PREF_PREFIX + "alice", "张三");
        m.put(LegacyDisplayNames.PREF_PREFIX + "bob", "李四");
        // 其他设置键：**绝不能被选中**
        m.put("pref_overlay_enabled", Boolean.TRUE);
        m.put("pref_last_preflight", "2026-09-15");
        m.put("pref_events_visible", Boolean.FALSE);
        m.put("pref_detect_interval", 500);
        // 边界：前缀少一个下划线，不应匹配
        m.put("pref_person_name", "这不是映射键");
        return m;
    }

    /** 只选中「显示名前缀」的键——不得误伤任何其他设置。 */
    @Test
    public void selectsOnlyDisplayNameKeys() {
        Set<String> keys = LegacyDisplayNames.selectKeysForClearing(prefs());
        assertEquals(2, keys.size());
        assertTrue(keys.contains(LegacyDisplayNames.PREF_PREFIX + "alice"));
        assertTrue(keys.contains(LegacyDisplayNames.PREF_PREFIX + "bob"));
        for (String k : keys) {
            assertTrue("只应选中显示名前缀", k.startsWith(LegacyDisplayNames.PREF_PREFIX));
        }
    }

    /** 其他设置键必须全部幸免（清除流程误伤即用户配置丢失）。 */
    @Test
    public void otherSettingsAreNeverSelected() {
        Set<String> keys = LegacyDisplayNames.selectKeysForClearing(prefs());
        assertFalse(keys.contains("pref_overlay_enabled"));
        assertFalse(keys.contains("pref_last_preflight"));
        assertFalse(keys.contains("pref_events_visible"));
        assertFalse(keys.contains("pref_detect_interval"));
        assertFalse("前缀少下划线的键不属于映射", keys.contains("pref_person_name"));
    }

    /** 前缀匹配但值不是 String 的键不选中（防御异常写入）。 */
    @Test
    public void nonStringValuesUnderPrefixAreNotSelected() {
        Map<String, Object> m = new LinkedHashMap<>();
        m.put(LegacyDisplayNames.PREF_PREFIX + "weird", 42);
        assertTrue(LegacyDisplayNames.selectKeysForClearing(m).isEmpty());
    }

    /** 空串值算「纯残留」：会被清除选中，但**不**触发对用户的提示。 */
    @Test
    public void emptyValueIsResidueNotRetainedName() {
        Map<String, Object> m = new LinkedHashMap<>();
        m.put(LegacyDisplayNames.PREF_PREFIX + "empty", "");
        m.put(LegacyDisplayNames.PREF_PREFIX + "blank", "   ");

        assertEquals("空残留应被清除选中", 2, LegacyDisplayNames.selectKeysForClearing(m).size());
        assertFalse("空残留不含 PII，不应打扰用户", LegacyDisplayNames.hasRetainedNames(m));
        assertEquals(0, LegacyDisplayNames.retainedCount(m));
    }

    /** 是否提示：只由「非空值」决定。 */
    @Test
    public void retainedDetectionDependsOnNonEmptyValue() {
        assertFalse("无历史键时不提示", LegacyDisplayNames.hasRetainedNames(new LinkedHashMap<>()));

        Map<String, Object> m = new LinkedHashMap<>();
        m.put(LegacyDisplayNames.PREF_PREFIX + "alice", "张三");
        assertTrue(LegacyDisplayNames.hasRetainedNames(m));
        assertEquals(1, LegacyDisplayNames.retainedCount(m));
    }

    /** 前缀常量必须带结尾下划线（防止有人改成 `pref_person_name` 而开始误伤同名键）。 */
    @Test
    public void prefixEndsWithUnderscore() {
        assertTrue("前缀须以 _ 结尾，避免匹配到同名族键", LegacyDisplayNames.PREF_PREFIX.endsWith("_"));
    }
}

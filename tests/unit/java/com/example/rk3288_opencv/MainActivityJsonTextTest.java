package com.example.rk3288_opencv;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

/**
 * MainActivity.JsonText 顶层键取值回归测试（审计 #4）。
 *
 * <p>历史实现用整串 {@code indexOf} 取首个 {@code "key"}，若 native 在
 * {@code accelerators.<cap>.*} 内先输出同名键（如 {@code effective}/{@code reason}），
 * 会被误当管道级字段读取。此处锁定「只取顶层键」语义。
 *
 * <p>直接访问静态嵌套类 {@link MainActivity.JsonText} 不会初始化 MainActivity，
 * 因此不触发其 {@code System.loadLibrary}，可在纯 JVM 运行。
 */
public class MainActivityJsonTextTest {

    @Test
    public void prefersTopLevelKeyOverEarlierNestedKey() {
        String json = "{"
                + "\"accelerators\":{\"mpp\":{\"effective\":true,\"reason\":\"nested\"}},"
                + "\"effective\":false,\"reason\":\"pipeline\""
                + "}";
        assertEquals("false", MainActivity.JsonText.extractRawValue(json, "effective"));
        assertEquals("\"pipeline\"", MainActivity.JsonText.extractRawValue(json, "reason"));
    }

    @Test
    public void returnsNullWhenKeyOnlyExistsNested() {
        String json = "{\"accelerators\":{\"mpp\":{\"effective\":true,\"reason\":\"nested\"}}}";
        assertNull(MainActivity.JsonText.extractRawValue(json, "effective"));
        assertNull(MainActivity.JsonText.extractRawValue(json, "reason"));
    }

    @Test
    public void ignoresKeyTextInsideStringValues() {
        String json = "{\"note\":\"\\\"reason\\\" 不是键\",\"reason\":\"top\"}";
        assertEquals("\"top\"", MainActivity.JsonText.extractRawValue(json, "reason"));
    }

    @Test
    public void readsTopLevelPrimitivesAndEscapedStrings() {
        String json = "{\"ok\":true,\"count\":3,\"p95\":12.5,\"id\":\"a\\\"b\"}";
        assertEquals("true", MainActivity.JsonText.extractRawValue(json, "ok"));
        assertEquals("3", MainActivity.JsonText.extractRawValue(json, "count"));
        assertEquals("12.5", MainActivity.JsonText.extractRawValue(json, "p95"));
        assertEquals("\"a\\\"b\"", MainActivity.JsonText.extractRawValue(json, "id"));
    }

    @Test
    public void worksOnIsolatedCapabilityObjectBody() {
        // capObject 抽出的子对象体仍是其自身顶层，reason/effective 必须可取。
        String capBody = "{\"compiled\":false,\"effective\":true,\"reason\":\"missing_dependency\"}";
        assertEquals("true", MainActivity.JsonText.extractRawValue(capBody, "effective"));
        assertEquals("\"missing_dependency\"", MainActivity.JsonText.extractRawValue(capBody, "reason"));
    }

    @Test
    public void handlesNullAndMissingInputs() {
        assertNull(MainActivity.JsonText.extractRawValue(null, "reason"));
        assertNull(MainActivity.JsonText.extractRawValue("{\"a\":1}", null));
        assertNull(MainActivity.JsonText.extractRawValue("{\"a\":1}", "missing"));
    }
}

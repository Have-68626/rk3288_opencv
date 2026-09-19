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

    // ═══════════════════════════════════════════════════════════════════════════
    // §2.5-G：对象取值路径的顶层纪律
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // 旧 jsonExtractObject 用整串 indexOf 取**首个** "key"，与 extractRawValue 的顶层纪律不一致。
    // 下面前两条即为**判别性用例**：旧实现在它们上面取到的是**错的对象**（注释里写明错在哪）。

    /** ① 更早处存在同名**嵌套**键时，必须取顶层那个。 */
    @Test
    public void extractObjectPrefersTopLevelKeyOverEarlierNestedKey() {
        String accBody = "{\"other\":{\"mpp\":{\"compiled\":false}},\"mpp\":{\"compiled\":true}}";
        // 旧实现首个 "\"mpp\"" 命中 other 的**内层键**，随后取其后首个 '{' ⇒ 返回内层对象体
        // {"compiled":false}（A1 审计实测确认返回值不含外层 "mpp": 包装）—— 即取错了对象。
        assertEquals("{\"compiled\":true}", MainActivity.JsonText.extractObject(accBody, "mpp"));
    }

    /** ② 更早处有**字符串值恰好等于该键名**（值自身的引号构成 "key" 字面量）时，不得误取。 */
    @Test
    public void extractObjectIgnoresKeyNameAppearingAsStringValue() {
        String accBody = "{\"note\":\"mpp\",\"z\":{\"a\":1},\"mpp\":{\"compiled\":true}}";
        // 旧实现匹配到 note 的**值** "mpp"，随后取其后首个 '{' ⇒ 返回 {"a":1}
        assertEquals("{\"compiled\":true}", MainActivity.JsonText.extractObject(accBody, "mpp"));
    }

    /**
     * ③ {@code capObject} 的**预期**形态：顶层 accelerators 对象体 → 其内部同名 cap。
     *
     * <p>⚠️ 两条如实标注（A2 审计指出）：
     * <ol>
     *   <li><b>对旧实现同样通过</b> —— 它不是判别性用例，只是语义锁定，**不能**当作 §2.5-G 的证据；</li>
     *   <li>native 侧<b>当前并不输出 {@code accelerators}</b>（见 {@code JsonText.capObject} 的现状说明），
     *       故这是**前向兼容**形态，<b>不是</b>「实际调用形态」。</li>
     * </ol>
     */
    @Test
    public void extractObjectHandlesAcceleratorsThenCap() {
        String json = "{\"accelerators\":{\"mpp\":{\"effective\":true},\"ncnn\":{\"effective\":false}},"
                + "\"effective\":false}";
        String acc = MainActivity.JsonText.extractObject(json, "accelerators");
        assertEquals("{\"mpp\":{\"effective\":true},\"ncnn\":{\"effective\":false}}", acc);
        assertEquals("{\"effective\":true}", MainActivity.JsonText.extractObject(acc, "mpp"));
        assertEquals("{\"effective\":false}", MainActivity.JsonText.extractObject(acc, "ncnn"));
    }

    /** ④ 花括号配平必须忽略字符串内的括号（**对旧实现亦通过，非判别性**：旧循环体被逐字保留）。 */
    @Test
    public void extractObjectBalancesBracesIgnoringStringContent() {
        String json = "{\"accelerators\":{\"detail\":{\"a\":1},\"note\":\"}{\"}}";
        assertEquals("{\"detail\":{\"a\":1},\"note\":\"}{\"}",
                MainActivity.JsonText.extractObject(json, "accelerators"));
    }

    /** ⑤ 缺失键、值不是对象、以及 null 入参一律返回 null（旧实现在「值不是对象」时会取到**后面**那个对象的体）。 */
    @Test
    public void extractObjectReturnsNullForMissingOrNonObjectValue() {
        assertNull(MainActivity.JsonText.extractObject("{\"mpp\":\"disabled\"}", "mpp"));
        assertNull(MainActivity.JsonText.extractObject("{\"a\":{}}", "mpp"));
        assertNull(MainActivity.JsonText.extractObject("{\"mpp\":123,\"other\":{\"a\":1}}", "mpp"));
        assertNull(MainActivity.JsonText.extractObject(null, "mpp"));
        assertNull(MainActivity.JsonText.extractObject("{\"a\":{}}", null));
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // capObject：两条分支 + 回退（A1 审计指出此前**仓内无任何测试直接驱动**它）
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // ⚠️ **判别力如实标注**（兄弟代理 4 / 子代理 3 各自独立指出）：
    //   下面 **4 条对本次改动前的实现全部恒真** —— 旧 `capObject` 的**结构**与现在逐字相同
    //   （探针实测新旧返回完全一致），坏的只是它调用的 `jsonExtractObject`（对象键定位）。
    //   ⇒ 它们锁的是「分支选择与回退的预期语义」与「防未来某类回归」，
    //     **不能**当作「本次修复了 capObject 的 bug」的证据。
    //   若谁据「新增用例覆盖了两条分支」得出「改前 capObject 是坏的」，那是**过度归因**。
    //   本类真正的判别性证据集中在：extractObjectPrefersTopLevelKeyOverEarlierNestedKey、
    //   extractObjectIgnoresKeyNameAppearingAsStringValue、以及
    //   extractObjectReturnsNullForMissingOrNonObjectValue 里 `{"mpp":123,…}` 那一条断言。

    /** 分支 1：accelerators 存在且其内有同名 cap ⇒ 取嵌套那个（优先于顶层同名）。 */
    @Test
    public void capObjectPrefersNestedUnderAccelerators() {
        String json = "{\"accelerators\":{\"mpp\":{\"effective\":true}},\"mpp\":{\"effective\":false}}";
        assertEquals("{\"effective\":true}", MainActivity.JsonText.capObject(json, "mpp"));
    }

    /** 分支 2：完全无 accelerators ⇒ 回退顶层同名 cap。 */
    @Test
    public void capObjectFallsBackToTopLevelWhenAcceleratorsAbsent() {
        String json = "{\"mpp\":{\"effective\":true},\"effective\":false}";
        assertEquals("{\"effective\":true}", MainActivity.JsonText.capObject(json, "mpp"));
    }

    /** 分支 3：accelerators 存在但其内无该 cap ⇒ 同样回退顶层。 */
    @Test
    public void capObjectFallsBackToTopLevelWhenCapMissingUnderAccelerators() {
        String json = "{\"accelerators\":{\"ncnn\":{\"effective\":false}},\"mpp\":{\"effective\":true}}";
        assertEquals("{\"effective\":true}", MainActivity.JsonText.capObject(json, "mpp"));
    }

    /**
     * **现行生产 JSON 形态**：即 {@code NativeFaceBridge.getRuntimeStatus()} 的真实输出
     * （见 {@code jni/native_face_bridge.cpp} 的 {@code bridge_getRuntimeStatus}）——无
     * {@code accelerators}、也无任何 cap 对象 ⇒ capObject **恒返回 null**。
     * 这条同时钉住「前向兼容路径不是已生效的生产路径」这一事实，防止将来有人误以为 cap 读取已可用。
     *
     * <p>⚠️ 本条在本组 4 条中**最弱**（子代理 3 指出）：现行生产 JSON 里根本没有
     * {@code accelerators}/{@code mpp}/{@code ncnn}/{@code opencl} 这些键，任何「查键返回子串」的
     * 合理实现都会返回 null ⇒ 它近乎**准恒真**，只在实现荒谬到「凭空造对象」时才会红。
     * 保留它的价值在**文档与事实锁定**，而非判别力。
     */
    @Test
    public void capObjectReturnsNullOnCurrentProductionJsonShape() {
        String json = "{\"ok\":true,\"profileValidated\":true,\"profileLoaded\":true,\"profileId\":\"p1\","
                + "\"embeddingDim\":512,\"galleryLoaded\":true,\"galleryCount\":3,\"galleryDim\":512,"
                + "\"enrollmentState\":\"IDLE\",\"effective\":true,\"reason\":\"ok\"}";
        assertNull(MainActivity.JsonText.capObject(json, "mpp"));
        assertNull(MainActivity.JsonText.capObject(json, "ncnn"));
        assertNull(MainActivity.JsonText.capObject(json, "opencl"));
        assertNull(MainActivity.JsonText.capObject(null, "mpp"));
    }
}

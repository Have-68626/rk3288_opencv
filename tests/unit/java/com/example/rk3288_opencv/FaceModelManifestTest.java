package com.example.rk3288_opencv;

import org.junit.Test;

import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * FaceModelManifest（任务 14）单元测试：Java 侧 manifest v1 解析必须与 C++
 * {@code FaceRuntimeManifest::parseV1} 契约一致。
 */
public class FaceModelManifestTest {

    private static final String ABI = "arm64-v8a";

    /** 64 位 hex 常量。 */
    private static final String SHA_A = repeat('a');
    private static final String SHA_B = repeat('b');
    private static final String SHA_C = repeat('c');
    private static final String SHA_D = repeat('d');

    /** 一份合法 manifest v1，测试通过字符串替换派生非法用例。 */
    private static final String VALID = "{"
            + "\"schemaVersion\": 1,"
            + "\"profileId\": \"p1\","
            + "\"displayName\": \"Profile One\","
            + "\"supportedAbis\": [\"arm64-v8a\"],"
            + "\"detector\": {"
            + "  \"param\": {\"path\": \"scrfd.param\", \"sha256\": \"" + SHA_A + "\"},"
            + "  \"bin\": {\"path\": \"scrfd.bin\", \"sha256\": \"" + SHA_B + "\"},"
            + "  \"input\": \"in0\","
            + "  \"outputs\": [\"score_8\",\"score_16\",\"score_32\",\"bbox_8\",\"bbox_16\",\"bbox_32\",\"kps_8\",\"kps_16\",\"kps_32\"],"
            + "  \"inputWidth\": 640, \"inputHeight\": 640,"
            + "  \"decodeMode\": \"distance\", \"anchorsPerPos\": [2,2,2], \"outputLayout\": \"nchw\""
            + "},"
            + "\"embedder\": {"
            + "  \"param\": {\"path\": \"arcface.param\", \"sha256\": \"" + SHA_C + "\"},"
            + "  \"bin\": {\"path\": \"arcface.bin\", \"sha256\": \"" + SHA_D + "\"},"
            + "  \"input\": \"in0\", \"output\": \"out0\","
            + "  \"inputWidth\": 112, \"inputHeight\": 112, \"embeddingDim\": 512"
            + "},"
            + "\"detectionConfidence\": 0.55, \"nmsIou\": 0.40, \"identitySimilarity\": 0.45,"
            + "\"modelVersion\": \"1\", \"licenseId\": \"insightface-noncommercial\""
            + "}";

    @Test
    public void validManifestParsesAllContractFields() {
        FaceModelManifest.ParseResult r = FaceModelManifest.parse(VALID, ABI);
        assertTrue(r.message, r.isOk());
        FaceModelManifest.Profile p = r.profile;
        assertNotNull(p);
        assertEquals(1, p.schemaVersion);
        assertEquals("p1", p.profileId);
        assertEquals("Profile One", p.displayName);
        assertEquals(List.of("arm64-v8a"), p.supportedAbis);
        assertEquals("scrfd.param", p.detectorParam.relativePath);
        assertEquals(SHA_A, p.detectorParam.sha256);
        assertEquals("scrfd.bin", p.detectorBin.relativePath);
        assertEquals("arcface.param", p.embedderParam.relativePath);
        assertEquals("arcface.bin", p.embedderBin.relativePath);
        assertEquals("in0", p.detectorInput);
        assertEquals(9, p.detectorOutputs.size());
        assertEquals(640, p.detectorInputW);
        assertEquals("distance", p.detectorDecodeMode);
        assertEquals(2, p.detectorAnchorsPerPos[0]);
        assertEquals("nchw", p.detectorOutputLayout);
        assertEquals("out0", p.embedderOutput);
        assertEquals(112, p.embedderInputW);
        assertEquals(512, p.embeddingDim);
        assertEquals(0.55f, p.detectionConfidence, 1e-6);
        assertEquals("1", p.modelVersion);
        assertEquals("insightface-noncommercial", p.licenseId);
        assertEquals(4, p.allEntries().size());
    }

    @Test
    public void rejectsSchemaVersionOtherThanExactOne() {
        assertEquals(FaceModelManifest.Status.UNSUPPORTED_SCHEMA,
                FaceModelManifest.parse(VALID.replace("\"schemaVersion\": 1", "\"schemaVersion\": 2"), ABI).status);
        // 1.5 必须被拒绝（不能按 int 截断成 1）。
        assertEquals(FaceModelManifest.Status.UNSUPPORTED_SCHEMA,
                FaceModelManifest.parse(VALID.replace("\"schemaVersion\": 1", "\"schemaVersion\": 1.5"), ABI).status);
        assertEquals(FaceModelManifest.Status.UNSUPPORTED_SCHEMA,
                FaceModelManifest.parse(VALID.replace("\"schemaVersion\": 1,", ""), ABI).status);
    }

    @Test
    public void rejectsAbiMismatchButAllowsNullAbiForDisplay() {
        assertEquals(FaceModelManifest.Status.ABI_MISMATCH,
                FaceModelManifest.parse(VALID, "armeabi-v7a").status);
        // abi==null 仅用于展示已落盘配置，跳过 ABI 匹配。
        assertTrue(FaceModelManifest.parse(VALID, null).isOk());
    }

    @Test
    public void rejectsUnsafeModelPaths() {
        for (String bad : new String[]{"../scrfd.param", "/scrfd.param", "C:\\scrfd.param", "a/../../x"}) {
            FaceModelManifest.ParseResult r =
                    FaceModelManifest.parse(VALID.replace("\"scrfd.param\"", "\"" + bad.replace("\\", "\\\\") + "\""), ABI);
            assertEquals("应拒绝路径: " + bad, FaceModelManifest.Status.UNSAFE_PATH, r.status);
        }
    }

    @Test
    public void rejectsMalformedSha256() {
        FaceModelManifest.ParseResult r =
                FaceModelManifest.parse(VALID.replace(SHA_A, "xyz"), ABI);
        assertEquals(FaceModelManifest.Status.BAD_SHA256, r.status);
        assertEquals(FaceModelManifest.Status.BAD_SHA256,
                FaceModelManifest.parse(VALID.replace(SHA_A, repeat('a').substring(0, 63)), ABI).status);
    }

    @Test
    public void emptySha256IsAllowedAsNoPin() {
        FaceModelManifest.ParseResult r = FaceModelManifest.parse(VALID.replace(SHA_A, ""), ABI);
        assertTrue(r.message, r.isOk());
        assertTrue(r.profile.detectorParam.sha256.isEmpty());
    }

    @Test
    public void rejectsMissingRequiredFields() {
        assertEquals(FaceModelManifest.Status.MISSING_FIELD,
                FaceModelManifest.parse(VALID.replace("\"profileId\": \"p1\",", ""), ABI).status);
        assertEquals(FaceModelManifest.Status.MISSING_FIELD,
                FaceModelManifest.parse(VALID.replace("\"supportedAbis\": [\"arm64-v8a\"],", ""), ABI).status);
        assertEquals(FaceModelManifest.Status.MISSING_FIELD,
                FaceModelManifest.parse(VALID.replace("\"detector\": {", "\"detectorX\": {"), ABI).status);
    }

    /**
     * 导入校验：profileId 字符集。拒绝任何 C0 控制字符（{@code c < 0x20}，含 {@code \n \r \t}）
     * 与 DEL(0x7F)，以及**首字符或末字符为空格**(0x20)；放行 `=`、内部空格、`.`、`*`、`?`、非 ASCII。
     * 规则与 C++ 侧镜像逐字一致；返回 {@link FaceModelManifest.Status#MISSING_FIELD}
     * （{@code FaceModelManifest.Status} 无 MANIFEST_INVALID，且该方法紧邻 isBlank 分支；
     * 外层 {@code FaceModelManager} 会统一映射为 MANIFEST_INVALID）。
     */
    @Test
    public void importRejectsProfileIdWithControlCharsOrEdgeSpaces() {
        for (String bad : new String[]{"a\nb", "a\rb", "a\tb", " a", "a ", "ab"}) {
            FaceModelManifest.ParseResult r = parseWithProfileId(bad);
            assertEquals("应拒绝 profileId: " + describe(bad),
                    FaceModelManifest.Status.MISSING_FIELD, r.status);
            assertFalse("被拒时不得产出 profile", r.isOk());
        }
        for (String ok : new String[]{"rk3288_light", "a=b", "a b", "a.b"}) {
            FaceModelManifest.ParseResult r = parseWithProfileId(ok);
            assertTrue("应接受 profileId: " + describe(ok), r.isOk());
            assertEquals(ok, r.profile.profileId);
        }
    }

    /** 用指定 profileId（自动 JSON 转义）派生 VALID manifest 并解析。 */
    private static FaceModelManifest.ParseResult parseWithProfileId(String id) {
        return FaceModelManifest.parse(
                VALID.replace("\"profileId\": \"p1\"", "\"profileId\": " + jsonString(id)), ABI);
    }

    /** 将字符串编码为 JSON 字符串字面量（控制字符转 {@code \n}/\r/\t 或 Unicode 转义）。 */
    private static String jsonString(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 2).append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (c < 0x20 || c == 0x7F) {
                        sb.append(String.format("\\u%04x", (int) c));
                    } else {
                        sb.append(c);
                    }
            }
        }
        return sb.append('"').toString();
    }

    /** 控制字符可见化，便于失败信息定位。 */
    private static String describe(String s) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < 0x20 || c == 0x7F) {
                sb.append(String.format("\\u%04x", (int) c));
            } else {
                sb.append(c);
            }
        }
        return "[" + sb + "]";
    }

    @Test
    public void rejectsWrongDetectorOutputCount() {
        FaceModelManifest.ParseResult r =
                FaceModelManifest.parse(VALID.replace(",\"kps_32\"", ""), ABI);
        assertEquals(FaceModelManifest.Status.MISSING_FIELD, r.status);
    }

    @Test
    public void rejectsBadThresholdsAndSizes() {
        assertEquals(FaceModelManifest.Status.BAD_VALUE,
                FaceModelManifest.parse(VALID.replace("\"detectionConfidence\": 0.55", "\"detectionConfidence\": 1.5"), ABI).status);
        assertEquals(FaceModelManifest.Status.BAD_VALUE,
                FaceModelManifest.parse(VALID.replace("\"nmsIou\": 0.40", "\"nmsIou\": 0"), ABI).status);
        // anchorsPerPos 必须恰 3 个正整数（与 C++ parseAnchorsPerPos 一致）。
        assertEquals(FaceModelManifest.Status.BAD_VALUE,
                FaceModelManifest.parse(VALID.replace("\"anchorsPerPos\": [2,2,2]", "\"anchorsPerPos\": [2,2]"), ABI).status);
        assertEquals(FaceModelManifest.Status.BAD_VALUE,
                FaceModelManifest.parse(VALID.replace("\"decodeMode\": \"distance\"", "\"decodeMode\": \"magic\""), ABI).status);
    }

    @Test
    public void rejectsInvalidJsonAndNonObjectRoot() {
        assertEquals(FaceModelManifest.Status.INVALID_JSON, FaceModelManifest.parse("{", ABI).status);
        assertEquals(FaceModelManifest.Status.NOT_OBJECT, FaceModelManifest.parse("[]", ABI).status);
    }

    /**
     * 审计 #5：递归解析必须有深度上限，超限抛受控 {@link FaceModelManifest.Json.JsonException}
     * 而非 {@link StackOverflowError}；1MiB 深嵌套 manifest 不得击穿导入线程。
     */
    @Test
    public void deepNestingIsRejectedWithControlledErrorNotStackOverflow() throws Exception {
        int limit = FaceModelManifest.Json.MAX_DEPTH;
        assertNotNull("未超限的嵌套应正常解析", FaceModelManifest.parseJsonValue(nestedArrays(limit)));

        String tooDeep = nestedArrays(limit + 1);
        try {
            FaceModelManifest.parseJsonValue(tooDeep);
            fail("超深嵌套必须抛受控 JsonException");
        } catch (FaceModelManifest.Json.JsonException expected) {
            // 受控异常，而非 StackOverflowError。
        }
        // 公开 parse 入口必须转成 INVALID_JSON，不得崩溃。
        assertEquals(FaceModelManifest.Status.INVALID_JSON, FaceModelManifest.parse(tooDeep, ABI).status);
    }

    /** 生成 depth 层嵌套数组（最内层为数字 0）。 */
    private static String nestedArrays(int depth) {
        StringBuilder sb = new StringBuilder(depth * 2 + 1);
        for (int i = 0; i < depth; i++) {
            sb.append('[');
        }
        sb.append('0');
        for (int i = 0; i < depth; i++) {
            sb.append(']');
        }
        return sb.toString();
    }

    @Test
    public void missingOptionalFieldsUseContractDefaults() {
        String minimal = "{"
                + "\"schemaVersion\":1,\"profileId\":\"m\",\"displayName\":\"M\","
                + "\"supportedAbis\":[\"arm64-v8a\"],"
                + "\"detector\":{\"param\":{\"path\":\"d.param\"},\"bin\":{\"path\":\"d.bin\"}},"
                + "\"embedder\":{\"param\":{\"path\":\"e.param\"},\"bin\":{\"path\":\"e.bin\"}},"
                + "\"detectionConfidence\":0.5,\"nmsIou\":0.4,\"identitySimilarity\":0.4"
                + "}";
        FaceModelManifest.ParseResult r = FaceModelManifest.parse(minimal, ABI);
        assertTrue(r.message, r.isOk());
        assertEquals(640, r.profile.detectorInputW);
        assertEquals(112, r.profile.embedderInputW);
        assertEquals(512, r.profile.embeddingDim);
        assertEquals("delta", r.profile.detectorDecodeMode);
        assertEquals("auto", r.profile.detectorOutputLayout);
        assertEquals("1", r.profile.modelVersion);
        assertTrue(r.profile.licenseId.isEmpty());
        assertTrue(r.profile.detectorOutputs.isEmpty());
    }

    @Test
    public void relativePathSafetyMatchesCppSemantics() {
        assertTrue(FaceModelManifest.isSafeRelativePath("scrfd.param"));
        assertTrue(FaceModelManifest.isSafeRelativePath("sub/dir/model.bin"));
        assertTrue(FaceModelManifest.isSafeRelativePath("a..b/c"));
        assertFalse(FaceModelManifest.isSafeRelativePath("../x"));
        assertFalse(FaceModelManifest.isSafeRelativePath("a/../x"));
        assertFalse(FaceModelManifest.isSafeRelativePath("/abs"));
        assertFalse(FaceModelManifest.isSafeRelativePath("\\abs"));
        assertFalse(FaceModelManifest.isSafeRelativePath("C:/x"));
        assertFalse(FaceModelManifest.isSafeRelativePath(""));
    }

    @Test
    public void dirNameSafety() {
        assertTrue(FaceModelManifest.isSafeDirName("rk3288_light"));
        assertFalse(FaceModelManifest.isSafeDirName(""));
        assertFalse(FaceModelManifest.isSafeDirName(".."));
        assertFalse(FaceModelManifest.isSafeDirName("a/b"));
        assertFalse(FaceModelManifest.isSafeDirName("a\\b"));
        assertFalse(FaceModelManifest.isSafeDirName("C:x"));
        assertFalse(FaceModelManifest.isSafeDirName(null));
    }

    @Test
    public void sha256HexValidation() {
        assertTrue(FaceModelManifest.isSha256Hex(SHA_A));
        assertTrue(FaceModelManifest.isSha256Hex(SHA_A.toUpperCase()));
        assertFalse(FaceModelManifest.isSha256Hex(SHA_A.substring(0, 63)));
        assertFalse(FaceModelManifest.isSha256Hex("g" + SHA_A.substring(1)));
    }

    private static String repeat(char c) {
        StringBuilder sb = new StringBuilder(64);
        for (int i = 0; i < 64; i++) {
            sb.append(c);
        }
        return sb.toString();
    }
}

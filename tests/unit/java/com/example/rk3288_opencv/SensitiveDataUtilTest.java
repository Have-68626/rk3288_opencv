package com.example.rk3288_opencv;

import org.junit.Test;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;

public class SensitiveDataUtilTest {

    @Test
    public void personIdentityKeysAreMasked() {
        String identity = "private_identity_fixture";
        for (String key : new String[]{"personId", "person_id", "PERSON-ID"}) {
            String masked = SensitiveDataUtil.maskSensitiveData("迁移失败 " + key + "=" + identity);
            assertFalse("身份键的原始值不得留在日志中", masked.contains(identity));
        }
    }

    @Test
    public void testNullOrEmpty() {
        assertNull("Null input should return null", SensitiveDataUtil.maskSensitiveData(null));
        assertEquals("Empty string should return empty string", "", SensitiveDataUtil.maskSensitiveData(""));
    }

    @Test
    public void testNoSensitiveData() {
        String input = "Hello world! This is a test.";
        assertEquals("Text without sensitive data should remain unchanged", input, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskPhoneNumber_Single() {
        String input = "My phone number is 13812345678.";
        String expected = "My phone number is 138****5678.";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskPhoneNumber_Multiple() {
        String input = "Call me at 13987654321 or 13700001111.";
        String expected = "Call me at 139****4321 or 137****1111.";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskPhoneNumber_MixedSeparators() {
        String input = "Contact: 电话: 13812345678, mobile=13987654321; fax\t13700001111";
        String expected = "Contact: 电话: 138****5678, mobile=139****4321; fax\t137****1111";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskIdCard_Single() {
        String input = "My ID is 110101199003071234.";
        String expected = "My ID is 110101********1234.";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskIdCard_Multiple() {
        String input = "IDs: 110101199003071234 and 31010120000101567X.";
        String expected = "IDs: 110101********1234 and 310101********567X.";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskIdCard_MixedSeparators() {
        String input = "User [ID: 110101199003071234] verified. Next ID=31010120000101567X.";
        String expected = "User [ID: 110101********1234] verified. Next ID=310101********567X.";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskGps_Single() {
        String input = "Location is 39.9042, 116.4074.";
        String expected = "Location is ***, ***.";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskGps_Multiple() {
        String input = "Points: 39.9042, 116.4074 to 31.2304, 121.4737.";
        String expected = "Points: ***, *** to ***, ***.";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskGps_MixedSeparators() {
        String input = "GPS=31.2304,121.4737|lat/lng(39.9042,  116.4074)";
        String expected = "GPS=***, ***|lat/lng(***, ***)";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMixedSensitiveData() {
        String input = "User: 13812345678, ID: 110101199003071234, Loc: 39.9042, 116.4074";
        String expected = "User: 138****5678, ID: 110101********1234, Loc: ***, ***";
        assertEquals(expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testAlreadyMaskedText() {
        String input = "My phone is 138****5678 and ID is 110101********1234 and GPS is ***, ***.";
        assertEquals("Already masked text should remain stable", input, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testMaskSecretKey_CompositeKey() {
        String input = "upload secret_key=superSecretVal done";
        String expected = "upload secret_key=su***al done";
        String result = SensitiveDataUtil.maskSensitiveData(input);
        assertEquals("Composite key secret_key value must be masked", expected, result);
        assertFalse("secret_key value must not leak", result.contains("superSecretVal"));
    }

    @Test
    public void testMaskClientSecret_CompositeKey() {
        String input = "client_secret=clientVal";
        String expected = "client_secret=cl***al";
        String result = SensitiveDataUtil.maskSensitiveData(input);
        assertEquals("Composite key client_secret value must be masked", expected, result);
        assertFalse("client_secret value must not leak", result.contains("clientVal"));
    }

    @Test
    public void testMaskAuthorizationBearer_JwtBody() {
        String jwt = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIiwiYXVkIjoiY2xpZW50X2lkIiwiZXhwIjoxOTc4MDIwMDAwfQ";
        String input = "Authorization: Bearer " + jwt;
        String expected = "Authorization: Bearer ey***fQ";
        String result = SensitiveDataUtil.maskSensitiveData(input);
        assertEquals("JWT body must be masked, Bearer prefix preserved", expected, result);
        assertFalse("JWT body must not leak", result.contains(jwt));
    }

    @Test
    public void testMaskPassword_Kv() {
        String input = "password=myP@ssw0rd";
        String expected = "password=my***rd";
        String result = SensitiveDataUtil.maskSensitiveData(input);
        assertEquals("password value must be masked", expected, result);
        assertFalse("password value must not leak", result.contains("myP@ssw0rd"));
    }

    @Test
    public void testMaskToken_Kv() {
        String input = "token=abc";
        String expected = "token=***";
        String result = SensitiveDataUtil.maskSensitiveData(input);
        assertEquals("Short token value must be fully masked", expected, result);
        assertFalse("token value must not leak", result.contains("abc"));
    }

    @Test
    public void testMaskEmail() {
        String input = "contact me at alice@example.com now";
        String expected = "contact me at ***@*** now";
        assertEquals("email must be masked", expected, SensitiveDataUtil.maskSensitiveData(input));
    }

    @Test
    public void testChineseTextUnchanged() {
        String input = "人脸识别初始化成功，设备已就绪。";
        assertEquals("Chinese text without sensitive data must remain unchanged",
                input, SensitiveDataUtil.maskSensitiveData(input));
    }
}

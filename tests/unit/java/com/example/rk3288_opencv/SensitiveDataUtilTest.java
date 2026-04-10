package com.example.rk3288_opencv;

import org.junit.Test;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

public class SensitiveDataUtilTest {

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
}

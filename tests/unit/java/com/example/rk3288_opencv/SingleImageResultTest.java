package com.example.rk3288_opencv;

import org.junit.Test;
import static org.junit.Assert.assertEquals;

public class SingleImageResultTest {
    @Test public void displaysOnlyThresholdAcceptedIdentity() {
        String json = "{\"ok\":true,\"decision\":{\"passNow\":true},\"TopK\":{\"hits\":[{\"personId\":\"synthetic_person\"}]}}";
        assertEquals("识别通过：synthetic_person", SingleImageResult.summary(json));
        assertEquals("未匹配到已注册人员", SingleImageResult.summary(json.replace("passNow\":true", "passNow\":false")));
    }
    @Test public void failureNeverDisplaysNativePathsOrDetailedPayload() {
        assertEquals("图片识别失败，请检查模型是否已激活及图片是否有效",
                SingleImageResult.summary("{\"ok\":false,\"message\":\"private_media_path\"}"));
        assertEquals("未检测到人脸", SingleImageResult.summary("{\"ok\":true,\"face\":{\"hasFace\":false}}"));
        assertEquals("图片识别失败：结果无效", SingleImageResult.summary("invalid"));
    }
}

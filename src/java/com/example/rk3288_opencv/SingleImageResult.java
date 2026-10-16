package com.example.rk3288_opencv;

import java.util.List;
import java.util.Map;

/** 仅提取可展示结论，完整推理 JSON 与媒体路径不进入日志或界面。 */
final class SingleImageResult {
    private SingleImageResult() {}

    static String summary(String json) {
        try {
            Map<String, Object> root = FaceModelManifest.asObject(FaceModelManifest.parseJsonValue(json));
            if (root == null || !Boolean.TRUE.equals(FaceModelManifest.asBoolean(root.get("ok")))) {
                return "图片识别失败，请检查模型是否已激活及图片是否有效";
            }
            Map<String, Object> detection = FaceModelManifest.asObject(root.get("face"));
            if (detection != null && Boolean.FALSE.equals(FaceModelManifest.asBoolean(detection.get("hasFace")))) {
                return "未检测到人脸";
            }
            Map<String, Object> decision = FaceModelManifest.asObject(root.get("decision"));
            Map<String, Object> top = FaceModelManifest.asObject(root.get("TopK"));
            List<Object> hits = top == null ? null : FaceModelManifest.asArray(top.get("hits"));
            if (decision != null && Boolean.TRUE.equals(FaceModelManifest.asBoolean(decision.get("passNow")))
                    && hits != null && !hits.isEmpty()) {
                Map<String, Object> hit = FaceModelManifest.asObject(hits.get(0));
                String id = hit == null ? null : FaceModelManifest.asString(hit.get("personId"));
                if (id != null && !id.isEmpty()) return "识别通过：" + id;
            }
            return "未匹配到已注册人员";
        } catch (Exception e) {
            return "图片识别失败：结果无效";
        }
    }
}

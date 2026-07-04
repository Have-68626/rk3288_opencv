#include "pipeline/TrackCoordinator.h"

#include <iostream>
#include <vector>

// 输入单个未认证脸 -> 返回 1 个 track, id=1, stableId="Unknown"
bool test_tc_creates_new_track_for_first_face() {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    f.confidence = 0.85f;
    f.isAuthenticated = false;
    f.identityId = "";
    faces.push_back(f);

    auto tracks = tc.update(faces, 1000);
    if (tracks.size() != 1) {
        std::cout << "FAIL: expected 1 track, got " << tracks.size() << std::endl;
        return false;
    }
    if (tracks[0].trackId != 1) {
        std::cout << "FAIL: expected trackId=1, got " << tracks[0].trackId << std::endl;
        return false;
    }
    if (tracks[0].stableId != "Unknown") {
        std::cout << "FAIL: expected stableId=Unknown, got " << tracks[0].stableId << std::endl;
        return false;
    }
    if (tracks[0].lastSeenMs != 1000) {
        std::cout << "FAIL: expected lastSeenMs=1000, got " << tracks[0].lastSeenMs << std::endl;
        return false;
    }
    return true;
}

// 输入脸在 1000ms -> 2000ms 后无输入 -> TTL 清理 -> 返回空
bool test_tc_removes_stale_tracks() {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    faces.push_back(f);

    auto tracks = tc.update(faces, 1000);
    if (tracks.size() != 1) {
        std::cout << "FAIL: expected 1 track after first update" << std::endl;
        return false;
    }

    // 500ms 后（距上次 500ms < TTL 1200ms），应还在
    auto tracks2 = tc.update({}, 1500);
    if (tracks2.size() != 1) {
        std::cout << "FAIL: expected 1 track at 1500ms (within TTL), got " << tracks2.size() << std::endl;
        return false;
    }

    // 再 800ms 后（距上次 1300ms > TTL 1200ms），应清除
    auto tracks3 = tc.update({}, 2300);
    if (!tracks3.empty()) {
        std::cout << "FAIL: expected 0 tracks at 2300ms (past TTL), got " << tracks3.size() << std::endl;
        return false;
    }

    return true;
}

// 连续两帧重叠的人脸保持同 trackId
bool test_tc_iou_matches_consecutive_frames() {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    f.isAuthenticated = true;
    f.identityId = "alice";
    f.confidence = 0.92f;
    faces.push_back(f);

    auto tracks1 = tc.update(faces, 1000);
    if (tracks1.size() != 1 || tracks1[0].trackId != 1) {
        std::cout << "FAIL: expected trackId=1 on first frame" << std::endl;
        return false;
    }
    if (tracks1[0].stableId != "alice") {
        std::cout << "FAIL: expected stableId=alice, got " << tracks1[0].stableId << std::endl;
        return false;
    }

    // 第二帧，人脸略偏移（高 IoU）
    pipeline::DetectedFace f2;
    f2.bbox = cv::Rect(12, 22, 100, 150);
    f2.isAuthenticated = true;
    f2.identityId = "alice";
    f2.confidence = 0.93f;
    std::vector<pipeline::DetectedFace> faces2 = {f2};

    auto tracks2 = tc.update(faces2, 1100);
    if (tracks2.size() != 1) {
        std::cout << "FAIL: expected 1 track on second frame" << std::endl;
        return false;
    }
    if (tracks2[0].trackId != 1) {
        std::cout << "FAIL: expected same trackId=1, got " << tracks2[0].trackId << std::endl;
        return false;
    }
    if (tracks2[0].stableId != "alice") {
        std::cout << "FAIL: expected stableId=alice, got " << tracks2[0].stableId << std::endl;
        return false;
    }
    if (tracks2[0].lastSeenMs != 1100) {
        std::cout << "FAIL: expected lastSeenMs=1100, got " << tracks2[0].lastSeenMs << std::endl;
        return false;
    }

    return true;
}

// 输入已认证人脸认证身份升级 "Unknown" -> 首次认证
bool test_tc_authenticated_upgrades_unknown() {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    f.isAuthenticated = false;
    f.identityId = "";
    faces.push_back(f);

    auto tracks1 = tc.update(faces, 1000);
    if (tracks1[0].stableId != "Unknown") {
        std::cout << "FAIL: expected Unknown before auth, got " << tracks1[0].stableId << std::endl;
        return false;
    }

    // 第二帧认证成功
    pipeline::DetectedFace f2;
    f2.bbox = cv::Rect(10, 20, 100, 150);
    f2.isAuthenticated = true;
    f2.identityId = "bob";
    f2.confidence = 0.88f;
    std::vector<pipeline::DetectedFace> faces2 = {f2};
    auto tracks2 = tc.update(faces2, 1100);

    if (tracks2[0].stableId != "bob") {
        std::cout << "FAIL: expected stableId=bob after auth, got " << tracks2[0].stableId << std::endl;
        return false;
    }
    if (tracks2[0].stableConfidence < 0.87f) {
        std::cout << "FAIL: expected stableConfidence≈0.88, got " << tracks2[0].stableConfidence << std::endl;
        return false;
    }

    return true;
}

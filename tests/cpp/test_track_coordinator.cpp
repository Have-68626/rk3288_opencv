#include "pipeline/TrackCoordinator.h"
#include <gtest/gtest.h>

#include <vector>

TEST(TrackCoordinator, CreatesNewTrackForFirstFace) {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    f.confidence = 0.85f;
    f.isAuthenticated = false;
    f.identityId = "";
    faces.push_back(f);

    auto tracks = tc.update(faces, 1000);
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks[0].trackId, 1);
    EXPECT_EQ(tracks[0].stableId, "Unknown");
    EXPECT_EQ(tracks[0].lastSeenMs, 1000);
}

TEST(TrackCoordinator, RemovesStaleTracks) {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    faces.push_back(f);

    auto tracks = tc.update(faces, 1000);
    ASSERT_EQ(tracks.size(), 1);

    // 500ms later (500ms < TTL 1200ms), track should persist
    auto tracks2 = tc.update({}, 1500);
    ASSERT_EQ(tracks2.size(), 1);

    // 800ms more (1300ms > TTL 1200ms), track should be removed
    auto tracks3 = tc.update({}, 2300);
    EXPECT_TRUE(tracks3.empty());
}

TEST(TrackCoordinator, IoUMatchesConsecutiveFrames) {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    f.isAuthenticated = true;
    f.identityId = "alice";
    f.confidence = 0.92f;
    faces.push_back(f);

    auto tracks1 = tc.update(faces, 1000);
    ASSERT_EQ(tracks1.size(), 1);
    EXPECT_EQ(tracks1[0].trackId, 1);
    EXPECT_EQ(tracks1[0].stableId, "alice");

    // Second frame, slightly shifted face (high IoU)
    pipeline::DetectedFace f2;
    f2.bbox = cv::Rect(12, 22, 100, 150);
    f2.isAuthenticated = true;
    f2.identityId = "alice";
    f2.confidence = 0.93f;
    std::vector<pipeline::DetectedFace> faces2 = {f2};

    auto tracks2 = tc.update(faces2, 1100);
    ASSERT_EQ(tracks2.size(), 1);
    EXPECT_EQ(tracks2[0].trackId, 1);
    EXPECT_EQ(tracks2[0].stableId, "alice");
    EXPECT_EQ(tracks2[0].lastSeenMs, 1100);
}

TEST(TrackCoordinator, AuthenticatedUpgradesUnknown) {
    pipeline::TrackCoordinator tc;
    std::vector<pipeline::DetectedFace> faces;
    pipeline::DetectedFace f;
    f.bbox = cv::Rect(10, 20, 100, 150);
    f.isAuthenticated = false;
    f.identityId = "";
    faces.push_back(f);

    auto tracks1 = tc.update(faces, 1000);
    ASSERT_EQ(tracks1.size(), 1);
    EXPECT_EQ(tracks1[0].stableId, "Unknown");

    // Second frame: authentication succeeds
    pipeline::DetectedFace f2;
    f2.bbox = cv::Rect(10, 20, 100, 150);
    f2.isAuthenticated = true;
    f2.identityId = "bob";
    f2.confidence = 0.88f;
    std::vector<pipeline::DetectedFace> faces2 = {f2};
    auto tracks2 = tc.update(faces2, 1100);

    ASSERT_EQ(tracks2.size(), 1);
    EXPECT_EQ(tracks2[0].stableId, "bob");
    EXPECT_GT(tracks2[0].stableConfidence, 0.87f);
}

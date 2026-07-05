#include "FrameInputChannel.h"
#include <gtest/gtest.h>

namespace {
ExternalFrame mkFrame(int w, int h, int64_t ts) {
    ExternalFrame f;
    f.format = ExternalFrameFormat::NV21;
    f.width = w;
    f.height = h;
    f.meta.timestampNs = ts;
    f.nv21RowStrideY = w;
    f.nv21.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3 / 2);
    return f;
}
}  // namespace

TEST(FrameInputChannel, LatestOnlyKeepsNewest) {
    FrameInputChannel ch;
    ch.configure(FrameBackpressureMode::LatestOnly, 1);

    ch.push(mkFrame(2, 2, 100));
    ch.push(mkFrame(2, 2, 200));

    ExternalFrame out;
    ASSERT_TRUE(ch.tryPop(out));
    EXPECT_EQ(out.meta.timestampNs, 200);

    const auto s = ch.stats();
    EXPECT_EQ(s.pushed, 2);
    EXPECT_EQ(s.popped, 1);
    EXPECT_EQ(s.dropped, 1);
}

TEST(FrameInputChannel, BoundedQueueDropsOldest) {
    FrameInputChannel ch;
    ch.configure(FrameBackpressureMode::BoundedQueue, 2);

    ch.push(mkFrame(2, 2, 100));
    ch.push(mkFrame(2, 2, 200));
    ch.push(mkFrame(2, 2, 300));

    ExternalFrame out1;
    ExternalFrame out2;
    ASSERT_TRUE(ch.tryPop(out1));
    ASSERT_TRUE(ch.tryPop(out2));
    EXPECT_EQ(out1.meta.timestampNs, 200);
    EXPECT_EQ(out2.meta.timestampNs, 300);

    const auto s = ch.stats();
    EXPECT_EQ(s.pushed, 3);
    EXPECT_EQ(s.popped, 2);
    EXPECT_EQ(s.dropped, 1);
}

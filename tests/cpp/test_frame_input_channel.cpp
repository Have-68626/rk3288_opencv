﻿#include "FrameInputChannel.h"


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

bool test_frame_input_latest_only_keeps_newest() {
    FrameInputChannel ch;
    ch.configure(FrameBackpressureMode::LatestOnly, 1);

    ch.push(mkFrame(2, 2, 100));
    ch.push(mkFrame(2, 2, 200));

    ExternalFrame out;
    if (!ch.tryPop(out)) return false;
    if (out.meta.timestampNs != 200) return false;

    const auto s = ch.stats();
    if (s.pushed != 2) return false;
    if (s.popped != 1) return false;
    if (s.dropped != 1) return false;

    return true;
}

bool test_frame_input_bounded_queue_drops_oldest() {
    FrameInputChannel ch;
    ch.configure(FrameBackpressureMode::BoundedQueue, 2);

    ch.push(mkFrame(2, 2, 100));
    ch.push(mkFrame(2, 2, 200));
    ch.push(mkFrame(2, 2, 300));

    ExternalFrame out1;
    ExternalFrame out2;
    if (!ch.tryPop(out1)) return false;
    if (!ch.tryPop(out2)) return false;
    if (out1.meta.timestampNs != 200) return false;
    if (out2.meta.timestampNs != 300) return false;

    const auto s = ch.stats();
    if (s.pushed != 3) return false;
    if (s.popped != 2) return false;
    if (s.dropped != 1) return false;

    return true;
}


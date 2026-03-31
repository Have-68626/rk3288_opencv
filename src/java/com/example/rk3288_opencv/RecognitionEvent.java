package com.example.rk3288_opencv;

import androidx.annotation.NonNull;

final class RecognitionEvent {
    final long ts;
    final String text;

    RecognitionEvent(long ts, @NonNull String text) {
        this.ts = ts;
        this.text = text;
    }
}


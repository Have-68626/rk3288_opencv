package com.example.rk3288_opencv;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class InferenceThrottleAutoTunerTest {
    @Test
    public void increasesIntervalOnBadStats_withCooldown() {
        InferenceThrottleAutoTuner t = new InferenceThrottleAutoTuner(150);

        StatsSnapshot bad = new StatsSnapshot(
                18.0,
                null,
                180.0,
                85.0,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null
        );

        assertTrue(t.update(bad, 10_000));
        assertEquals(180, t.getIntervalMs());

        assertFalse(t.update(bad, 10_500));
        assertEquals(180, t.getIntervalMs());

        assertTrue(t.update(bad, 12_000));
        assertEquals(210, t.getIntervalMs());
    }

    @Test
    public void decreasesIntervalOnGoodStats() {
        InferenceThrottleAutoTuner t = new InferenceThrottleAutoTuner(200);

        StatsSnapshot good = new StatsSnapshot(
                30.0,
                null,
                70.0,
                40.0,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null
        );

        assertTrue(t.update(good, 10_000));
        assertEquals(180, t.getIntervalMs());

        assertFalse(t.update(good, 10_500));
        assertEquals(180, t.getIntervalMs());

        assertTrue(t.update(good, 12_000));
        assertEquals(160, t.getIntervalMs());
    }
}


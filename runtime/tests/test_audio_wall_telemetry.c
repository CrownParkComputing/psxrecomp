#include "audio_wall_telemetry.h"
#include "audio_trace.h"

#include <math.h>
#include <stdio.h>

static int failures;

static void check_close(const char *name, double actual, double expected)
{
    if (fabs(actual - expected) > 0.000001) {
        fprintf(stderr, "FAIL: %s actual=%.9f expected=%.9f\n",
                name, actual, expected);
        failures++;
    }
}

int main(void)
{
    AudioTraceHostStats host;
    uint64_t uncovered = 0;
    uint64_t deadline;

    if (psx_audio_monotonic_delta(12, 5) != 7) {
        fprintf(stderr, "FAIL: monotonic delta\n");
        failures++;
    }
    if (psx_audio_monotonic_delta(5, 12) != 0) {
        fprintf(stderr, "FAIL: reset-safe delta\n");
        failures++;
    }

    check_close("SPU wall rate", psx_audio_wall_rate(44100, 1.0), 44100.0);
    check_close("CDDA wall rate", psx_audio_wall_rate(75, 1.0), 75.0);
    check_close("zero wall interval", psx_audio_wall_rate(75, 0.0), 0.0);

    check_close("A/V aligned",
                psx_audio_timeline_drift_ms(44100, 44100.0, 100, 100.0),
                0.0);
    check_close("audio ahead",
                psx_audio_timeline_drift_ms(46305, 44100.0, 100, 100.0),
                50.0);
    check_close("audio behind",
                psx_audio_timeline_drift_ms(41895, 44100.0, 100, 100.0),
                -50.0);
    check_close("host wall behind",
                psx_audio_wall_drift_ms(43218, 44100.0, 1.0),
                -20.0);

    /* SDL3 may split one 500-frame feed into a large and a small callback.
     * The deadline must cover the whole batch, not only the final chunk. */
    deadline = psx_audio_callback_coverage_step(
        0, 1000, 400, 1000000, 1000, &uncovered);
    if (deadline != 401000 || uncovered != 0) {
        fprintf(stderr, "FAIL: callback coverage initial\n");
        failures++;
    }
    deadline = psx_audio_callback_coverage_step(
        deadline, 1000, 100, 1000000, 1000, &uncovered);
    if (deadline != 501000 || uncovered != 0) {
        fprintf(stderr, "FAIL: callback coverage split feed\n");
        failures++;
    }
    deadline = psx_audio_callback_coverage_step(
        deadline, 551000, 100, 1000000, 1000, &uncovered);
    if (deadline != 651000 || uncovered != 50000) {
        fprintf(stderr, "FAIL: callback uncovered gap\n");
        failures++;
    }
    deadline = psx_audio_callback_coverage_step(
        0, 9000000, 100, 1000000, 1000, &uncovered);
    if (deadline != 9100000 || uncovered != 0) {
        fprintf(stderr, "FAIL: callback session re-anchor\n");
        failures++;
    }

    audio_trace_init();
    audio_trace_event(AUDIO_EV_RENDER, 128, 100);
    audio_trace_event(AUDIO_EV_RENDER, 128, 80);
    audio_trace_event(AUDIO_EV_PUMP_SKIP, 100, 0);
    audio_trace_event(AUDIO_EV_PUMP_SKIP, 100, 0);
    audio_trace_event(AUDIO_EV_UNDERRUN, 0, 0);
    audio_trace_event(AUDIO_EV_MUTE, 0, 0);
    audio_trace_event(AUDIO_EV_UNMUTE, 0, 0);
    audio_trace_get_host_stats(&host);
    if (host.pump_calls != 2 || host.pump_skips != 2 ||
        host.underruns != 1 || host.queue_hiwater != 100 ||
        host.queue_lowater != 80 || host.mute_events != 1 ||
        host.unmute_events != 1) {
        fprintf(stderr,
                "FAIL: host stats calls=%llu skips=%llu underruns=%llu "
                "hi=%u low=%u mute=%llu unmute=%llu\n",
                (unsigned long long)host.pump_calls,
                (unsigned long long)host.pump_skips,
                (unsigned long long)host.underruns,
                host.queue_hiwater, host.queue_lowater,
                (unsigned long long)host.mute_events,
                (unsigned long long)host.unmute_events);
        failures++;
    }

    if (failures) return 1;
    puts("PASS: wall-domain audio telemetry arithmetic");
    return 0;
}

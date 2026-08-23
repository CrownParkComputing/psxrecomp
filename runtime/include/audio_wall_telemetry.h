/*
 * audio_wall_telemetry.h - clock-domain-explicit audio telemetry arithmetic.
 *
 * Keep these helpers independent of SDL and runtime state so the reporting
 * math can be unit-tested without launching a title.  Counts are monotonic
 * production/consumption counters; callers choose the counter's domain and
 * label it at the output site.
 */
#ifndef PSX_AUDIO_WALL_TELEMETRY_H
#define PSX_AUDIO_WALL_TELEMETRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t psx_audio_monotonic_delta(uint64_t current,
                                                  uint64_t previous)
{
    return current >= previous ? current - previous : 0u;
}

static inline double psx_audio_wall_rate(uint64_t count, double wall_seconds)
{
    return wall_seconds > 0.0 ? (double)count / wall_seconds : 0.0;
}

/* Signed timeline difference: positive means the audio timeline is ahead. */
static inline double psx_audio_timeline_drift_ms(uint64_t audio_frames,
                                                  double audio_rate,
                                                  uint64_t reference_frames,
                                                  double reference_rate)
{
    if (audio_rate <= 0.0 || reference_rate <= 0.0)
        return 0.0;
    return ((double)audio_frames / audio_rate -
            (double)reference_frames / reference_rate) * 1000.0;
}

/* Signed wall drift: positive means the produced/consumed audio timeline is
 * ahead of elapsed host wall time. */
static inline double psx_audio_wall_drift_ms(uint64_t audio_frames,
                                              double audio_rate,
                                              double wall_seconds)
{
    if (audio_rate <= 0.0 || wall_seconds < 0.0)
        return 0.0;
    return ((double)audio_frames / audio_rate - wall_seconds) * 1000.0;
}

/* Advance a callback-consumption coverage deadline.  Multiple chunks emitted
 * by one SDL3 feed callback extend the same deadline, so the final small chunk
 * cannot make the next normal feed look late.  `uncovered_ticks` is zero on a
 * fresh/re-anchored session and otherwise reports only wall time not covered
 * by audio already handed to the host. */
static inline uint64_t psx_audio_callback_coverage_step(
    uint64_t previous_deadline, uint64_t now, uint64_t frames,
    uint64_t performance_frequency, uint64_t sample_rate,
    uint64_t *uncovered_ticks)
{
    uint64_t uncovered = 0;
    uint64_t base;
    uint64_t duration;

    if (previous_deadline != 0 && now > previous_deadline)
        uncovered = now - previous_deadline;
    if (uncovered_ticks) *uncovered_ticks = uncovered;

    base = previous_deadline > now ? previous_deadline : now;
    if (performance_frequency == 0 || sample_rate == 0 || frames == 0)
        return base;
    duration = frames * performance_frequency / sample_rate;
    return base + duration;
}

#ifdef __cplusplus
}
#endif

#endif /* PSX_AUDIO_WALL_TELEMETRY_H */

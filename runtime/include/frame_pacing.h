#ifndef FRAME_PACING_H
#define FRAME_PACING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
    #endif

    typedef struct FramePacer {
        uint64_t next_deadline;   /* perf-counter ticks; 0 = not started */
        double last_period_ms;    /* Guardamos el último periodo para detectar cambios */
        uint64_t cached_period;   /* El periodo ya masticado en ticks de CPU */
    } FramePacer;

    /* Función pura de decisión (sin cambios estructurales, pero optimizada internamente) */
    uint32_t frame_pacing_sleep_ms(uint64_t now, uint64_t deadline,
                                   uint64_t freq, uint64_t period);

    /* Bloquea hasta el próximo deadline usando esperas pasivas y activas optimizadas */
    void frame_pacer_wait(FramePacer *p, double period_ms);

    #ifdef __cplusplus
}
#endif

#endif /* FRAME_PACING_H */

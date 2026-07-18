#include "frame_pacing.h"

#ifndef FRAME_PACING_PURE_ONLY
#include <SDL.h>

/* Inteligencia de arquitectura: Evitamos quemar silicio innecesariamente */
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    #include <immintrin.h>
    #define CPU_PAUSE() _mm_pause()
#elif defined(__ARM_ARCH) || defined(__aarch64__)
    #define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define CPU_PAUSE() ((void)0)
#endif
#endif

uint32_t frame_pacing_sleep_ms(uint64_t now, uint64_t deadline,
                               uint64_t freq, uint64_t period) {
    if (now >= deadline) return 0;             /* compare BEFORE subtract */
    uint64_t remaining = deadline - now;       /* cannot underflow */
    if (remaining > period) remaining = period;/* hard cap: one frame max */

    /* freq == 0 eliminado de aquí. Se asume validación previa en el wrapper */
    uint64_t ms = (remaining * 1000u) / freq;
    if (ms < 2) return 0;                      /* sub-2ms: spin instead */
    return (uint32_t)(ms - 1);                 /* undershoot; spin covers rest */
}

#ifndef FRAME_PACING_PURE_ONLY

#define FRAME_PACER_CATCHUP_MAX_PERIODS 12u

void frame_pacer_wait(FramePacer *p, double period_ms) {
    uint64_t freq = SDL_GetPerformanceFrequency();
    SDL_assert(freq != 0); /* Validación única fuera del bucle caliente */

    /* Optimización de Caché: Solo calculamos con flotantes si cambia el modo de video */
    if (p->last_period_ms != period_ms) {
        p->cached_period = (uint64_t)((double)freq * (period_ms / 1000.0));
        p->last_period_ms = period_ms;
    }

    uint64_t period = p->cached_period;
    uint64_t now = SDL_GetPerformanceCounter();

    if (p->next_deadline == 0 ||
        now >= p->next_deadline + period * FRAME_PACER_CATCHUP_MAX_PERIODS) {
        p->next_deadline = now + period;
        return;
    }
    if (now >= p->next_deadline) {
        p->next_deadline += period;
        return;
    }

    for (;;) {
        now = SDL_GetPerformanceCounter();     /* ONE read per iteration */
        uint32_t ms = frame_pacing_sleep_ms(now, p->next_deadline, freq, period);
        if (ms == 0) break;
        SDL_Delay(ms);
    }

    /* El bucle de la muerte ahora es un bucle amigable con el planificador de la CPU */
    while (SDL_GetPerformanceCounter() < p->next_deadline) {
        CPU_PAUSE();
    }
    p->next_deadline += period;
}
#endif /* FRAME_PACING_PURE_ONLY */

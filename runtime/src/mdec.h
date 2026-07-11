#ifndef PSX_MDEC_H
#define PSX_MDEC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void mdec_init(void);
uint32_t mdec_read0(void);
void mdec_write0(uint32_t data);
uint32_t mdec_read1(void);
void mdec_write1(uint32_t data);
void mdec_dma0(uint32_t adr, uint32_t bcr, uint32_t chcr);
void mdec_dma1(uint32_t adr, uint32_t bcr, uint32_t chcr);
int mdec_recently_active(uint32_t within_frames);

uint32_t mdec_read(uint32_t addr);
void mdec_write(uint32_t addr, uint32_t data);

bool mdec_dma_read_ready(void);
bool mdec_dma_write_ready(void);
uint32_t mdec_dma_read_word(void);
void mdec_dma_write_word(uint32_t data);

void mdec_debug_dma_in_start(uint32_t addr, uint32_t total_words);
void mdec_debug_dma_in_end(uint32_t addr, uint32_t total_words);
void mdec_debug_dma_out_start(uint32_t addr, uint32_t total_words);
void mdec_debug_dma_out_end(uint32_t addr, uint32_t total_words);

typedef struct {
    uint32_t command;
    uint32_t expected_halfwords;
    uint32_t input_count;
    uint32_t output_size;
    uint32_t output_pos;
    uint32_t output_depth;
    uint32_t output_signed;
    uint32_t output_bit15;
    uint32_t busy;
    uint32_t input_full;
    uint32_t enable_dma_in;
    uint32_t enable_dma_out;
    uint32_t last_status;
    uint32_t decode_macroblocks;
    uint32_t decode_blocks;
    uint32_t decode_stop_reason;
    uint32_t decode_input_pos;
    uint32_t decode_input_end;
    uint32_t dma_in_words;
    uint32_t dma_out_words;
    uint32_t dma_read_underflows;
} MDECDebugState;

typedef struct {
    uint64_t seq;
    uint64_t frame;
    uint32_t kind;
    uint32_t command;
    uint32_t expected_halfwords;
    uint32_t input_count;
    uint32_t output_size;
    uint32_t output_pos;
    uint32_t macroblocks;
    uint32_t blocks;
    uint32_t stop_reason;
    uint32_t underruns;
    uint32_t value;
} MDECDebugEvent;

void mdec_debug_get_state(MDECDebugState *state);
uint32_t mdec_debug_get_event_total(void);
uint32_t mdec_debug_copy_events(uint32_t seq_lo, uint32_t seq_hi, MDECDebugEvent *out, uint32_t max_out);
void mdec_debug_clear(void);
uint32_t mdec_get_decode_count(void);

#ifdef __cplusplus
}
#endif

#endif
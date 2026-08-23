#include "fntrace.h"
#include "parity_trace.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

uint64_t s_frame_count = 17;
int g_insn_log_frozen = 0;
uint64_t g_psx_bail_flattened = 0;
uint32_t g_debug_current_func_addr = 0;

#ifndef PSX_SHIPPING_MINIMAL_DIAGNOSTICS
volatile uint32_t g_psx_last_fn_entry = 0;
#endif

static uint64_t s_cycle = 100;
static int s_mod_dispatches;
static int s_text_dispatches;
static int s_parity_armed;
static int s_parity_records;
static int s_game_notifications;
static int s_boot_captures;
static int s_baseline_clears;
static int s_scratch_clears;
static int s_bail_records;

uint64_t psx_get_cycle_count(void) { return s_cycle++; }
uint32_t psx_read_word(uint32_t addr) { return addr == 0x108u ? 0x200u : 0xABCDu; }
void mod_runtime_on_dispatch(uint32_t target) { (void)target; s_mod_dispatches++; }
void text_xlate_on_dispatch(CPUState* cpu, uint32_t target) {
    (void)cpu;
    (void)target;
    s_text_dispatches++;
}
int parity_trace_is_armed(void) { return s_parity_armed; }
void parity_trace_record(parity_kind_t kind, uint32_t pc, uint32_t ra,
                         uint32_t sp, uint32_t target,
                         parity_read_word_fn read_word, void* ctx) {
    (void)kind;
    (void)pc;
    (void)ra;
    (void)sp;
    (void)target;
    assert(read_word(ctx, 0x108u) == 0x200u);
    s_parity_records++;
}
void cdrom_notify_game_started(void) { s_game_notifications++; }
void boot_state_trigger_capture(const CPUState* cpu) { (void)cpu; s_boot_captures++; }
void dirty_ram_clear_image_baseline(void) { s_baseline_clears++; }
void memory_clear_low_boot_scratch(void) { s_scratch_clears++; }
int psx_game_address_in_text(uint32_t addr) { (void)addr; return 0; }
int psx_game_text_native_ok(uint32_t addr) { (void)addr; return 0; }
int dirty_ram_text_image_registered(void) { return 0; }
void psx_bail_record(uint32_t site_ra, uint32_t site_sp,
                     uint32_t wild_pc, uint32_t guest_sp) {
    (void)site_ra;
    (void)site_sp;
    (void)wild_pc;
    (void)guest_sp;
    s_bail_records++;
}

static uint32_t test_read_word(uint32_t addr) { return psx_read_word(addr); }

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.read_word = test_read_word;
    cpu.gpr[31] = 0x80001234u;
    cpu.gpr[29] = 0x8010FFF0u;

    fntrace_set_game_range(0x80010000u, 0x80020000u);
    fntrace_arm(0x80010000u);
    fntrace_record(&cpu, 0x80010000u);

    assert(s_mod_dispatches == 1);
    assert(s_text_dispatches == 1);
    assert(fntrace_is_game_started());
    assert(s_game_notifications == 1);
    assert(s_boot_captures == 1);
    assert(s_baseline_clears == 1);
    assert(s_scratch_clears == 1);
    assert(g_fntrace_seq == 1);
    assert(g_fntrace_ring[0].target == 0x80010000u);
    assert(g_fntrace_ring[0].ra == cpu.gpr[31]);

    s_parity_armed = 1;
    g_psx_bail_flattened = 1;
    cpu.gpr[29] = 0x80200010u;
    fntrace_record(&cpu, 0x80010000u);

    assert(s_mod_dispatches == 2);
    assert(s_text_dispatches == 2);
    assert(s_parity_records == 1);
    assert(g_fntrace_seq == 2);
    assert(s_game_notifications == 1);

    /* The shipping path still preserves the null-dispatch capture freeze used
     * by crash reporting; the armed fntrace filter simply leaves this target
     * out of its ring. */
    fntrace_record(&cpu, 0u);
    assert(g_insn_log_frozen == 1);
    assert(s_mod_dispatches == 3);
    assert(s_text_dispatches == 3);
    assert(s_parity_records == 2);
    assert(g_fntrace_seq == 2);

    debug_server_log_call_entry(0x80045678u);
#ifdef PSX_SHIPPING_MINIMAL_DIAGNOSTICS
    assert(g_disp_tail_seq == 0);
    assert(g_spdom_seq == 0);
    assert(s_bail_records == 0);
#else
    assert(g_disp_tail_seq == 3);
    assert(g_spdom_seq == 1);
    assert(s_bail_records == 1);
    assert(g_psx_last_fn_entry == 0x80045678u);
#endif

    return 0;
}

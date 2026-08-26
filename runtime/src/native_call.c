/* native_call.c — see native_call.h. */
#include "native_call.h"
#include "psx_runtime.h"

#include <stdio.h>
#include <string.h>

#define PSX_NATIVE_CALL_MAX 64

typedef struct {
    uint32_t        address;
    PSXNativeCallFn fn;
    void*           user;
} NativeCallSlot;

static NativeCallSlot s_slots[PSX_NATIVE_CALL_MAX];
static int            s_count = 0;

int psx_native_call_register(uint32_t address, PSXNativeCallFn fn, void* user) {
    if (!address || !fn) return 0;
    for (int i = 0; i < s_count; i++) {
        if (s_slots[i].address == address) {
            s_slots[i].fn = fn;
            s_slots[i].user = user;
            return 1;
        }
    }
    if (s_count >= PSX_NATIVE_CALL_MAX) {
        fprintf(stdout, "psxrecomp: native_call table full; 0x%08X not bound\n",
                address);
        return 0;
    }
    s_slots[s_count].address = address;
    s_slots[s_count].fn = fn;
    s_slots[s_count].user = user;
    s_count++;
    return 1;
}

int psx_native_call_count(void) { return s_count; }

int psx_native_call(CPUState* cpu, uint32_t address) {
    if (!cpu) return 0;
    for (int i = 0; i < s_count; i++) {
        if (s_slots[i].address != address) continue;
        if (!s_slots[i].fn(cpu, address, s_slots[i].user)) return 0;
        /* Same contract as a data-shard replay: publish $ra and let the
         * caller resume. The handler has already set $v0. */
        cpu->pc = cpu->gpr[31];
        return 1;
    }
    return 0;
}

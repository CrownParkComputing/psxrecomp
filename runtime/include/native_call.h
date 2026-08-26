/* native_call.h — replace a guest function with a native implementation.
 *
 * The recompiler emits, at the entry of every function listed in
 * [recompiler] native_funcs:
 *
 *     if (psx_native_call(cpu, 0xADDR)) return;
 *
 * A registered handler reads arguments from $a0..$a3, does the work, writes
 * its result to $v0 and returns 1; psx_native_call then publishes $ra as the
 * new pc so the caller resumes as if the guest body had run. Returning 0
 * leaves the guest body to execute normally — which is what makes this safe
 * to bring up incrementally: a handler can serve the cases it understands and
 * decline the rest, instead of an all-or-nothing patch.
 *
 * This is the same body-skip contract data shards use (data_shards.c), with a
 * native implementation in place of a recorded effect.
 */
#ifndef PSXRECOMP_NATIVE_CALL_H
#define PSXRECOMP_NATIVE_CALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Return 1 to skip the guest body ($v0 already set), 0 to run it. */
typedef int (*PSXNativeCallFn)(struct CPUState* cpu, uint32_t address, void* user);

/* Register a handler for a guest function address. One handler per address;
 * registering twice for the same address replaces the first. Returns 1 on
 * success. The address must also be in [recompiler] native_funcs or no hook
 * is emitted and the handler is never reached. */
int  psx_native_call_register(uint32_t address, PSXNativeCallFn fn, void* user);

/* Emitted hook. */
int  psx_native_call(struct CPUState* cpu, uint32_t address);

/* 1 when at least one handler is registered (for startup reporting). */
int  psx_native_call_count(void);

#ifdef __cplusplus
}
#endif
#endif

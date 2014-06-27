#ifndef _QEMU_DISAS_H
#define _QEMU_DISAS_H

#include "qemu-common.h"

/* Disassemble this for me please... (debugging). */
void disas(FILE *out, void *code, unsigned long size);
void target_disas(FILE *out, CPUArchState *env, target_ulong code,
                  target_ulong size, int flags);

void monitor_disas(Monitor *mon, CPUArchState *env,
                   target_ulong pc, int nb_insn, int is_physical, int flags);

/* Look up symbol for debugging purpose.  Returns "" if unknown. */
const char *lookup_symbol(target_ulong orig_addr);

#endif /* _QEMU_DISAS_H */

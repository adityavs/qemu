/* Declarations for use by hardware emulation.  */
#ifndef QEMU_HW_H
#define QEMU_HW_H

#include "qemu-common.h"

#ifdef CONFIG_USER_ONLY
#error Cannot include hw/hw.h from user emulation
#endif

#include "exec/cpu-common.h"
#include "exec/ioport.h"
#include "hw/irq.h"
#include "block/aio.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

typedef void QEMUResetHandler(void *opaque);

void qemu_register_reset(QEMUResetHandler *func, void *opaque);
void qemu_unregister_reset(QEMUResetHandler *func, void *opaque);

/* handler to set the boot_device order for a specific type of QEMUMachine */
/* return 0 if success */
typedef int QEMUBootSetHandler(void *opaque, const char *boot_order);
void qemu_register_boot_set(QEMUBootSetHandler *func, void *opaque);
int qemu_boot_set(const char *boot_order);

#endif

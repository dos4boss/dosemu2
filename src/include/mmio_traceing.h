#ifndef MMIO_TRACEING_H
#define MMIO_TRACEING_H

#include "emu.h"
#include "dosemu_debug.h"

#define MMIO_TRACEING_MAX_REGIONS 16
#define MMIO_READ 0x01
#define MMIO_WRITE 0x02
#define MMIO_BYTE 0x10
#define MMIO_WORD 0x20
#define MMIO_DWORD 0x40
#define MMIO_QWORD 0x80

extern void register_mmio_traceing(dosaddr_t startaddr, dosaddr_t stopaddr);
extern void mmio_check_and_trace(dosaddr_t addr, uint32_t value, uint8_t type);

#endif /* MMIO_TRACEING_H */

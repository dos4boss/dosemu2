#include "mmio_traceing.h"

struct mmio_address_range {
  dosaddr_t start, stop;
};

struct mmio_traceing_config
{
  struct mmio_address_range address_ranges[MMIO_TRACEING_MAX_REGIONS];
  unsigned valid_ranges;
  dosaddr_t min_addr, max_addr;
};

static struct mmio_traceing_config mmio_traceing_config;

void register_mmio_traceing(dosaddr_t startaddr, dosaddr_t stopaddr)
{
  if(stopaddr < startaddr)
  {
    error("MMIO: address order wrong.");
    return;
  }

  if(mmio_traceing_config.valid_ranges < MMIO_TRACEING_MAX_REGIONS - 1)
  {
    if(mmio_traceing_config.valid_ranges == 0)
    {
      mmio_traceing_config.min_addr = startaddr;
      mmio_traceing_config.max_addr = stopaddr;
    }
    else
    {
      if(startaddr < mmio_traceing_config.min_addr) mmio_traceing_config.min_addr = startaddr;
      if(stopaddr > mmio_traceing_config.max_addr) mmio_traceing_config.max_addr = stopaddr;
    }
    mmio_traceing_config.address_ranges[mmio_traceing_config.valid_ranges].start = startaddr;
    mmio_traceing_config.address_ranges[mmio_traceing_config.valid_ranges].stop = stopaddr;
    mmio_traceing_config.valid_ranges++;
  }
  else
    error("MMIO: Too many address regions to trace. Increase MMIO_TRACEING_MAX_REGIONS to allow some more...");
}

bool mmio_check(dosaddr_t addr)
{
  /* to not slow down too much for any other memory access (not in traceing region,
     MMIO is usually in some distance to RAM) */
  if((addr >= mmio_traceing_config.min_addr) && (addr <= mmio_traceing_config.max_addr))
  {
    for(unsigned k = 0; k < mmio_traceing_config.valid_ranges; k++)
    {
      if((addr >= mmio_traceing_config.address_ranges[k].start) &&
         (addr <= mmio_traceing_config.address_ranges[k].stop))
        return true;
    }
  }
  return false;
}

uint8_t mmio_trace_byte(dosaddr_t addr, uint8_t value, uint8_t type)
{
  switch(type)
  {
  case MMIO_READ:
    F_printf("MMIO: Reading byte at %X: %02X\n", addr, value);
    break;
  case MMIO_WRITE:
    F_printf("MMIO: Writing byte at %X: %02X\n", addr, value);
    break;
  default:
    F_printf("MMIO: Failed. Wrong arguments.");
  }
  return value;
}

uint16_t mmio_trace_word(dosaddr_t addr, uint16_t value, uint8_t type)
{
  switch(type)
  {
  case MMIO_READ:
    F_printf("MMIO: Reading word at %X: %04X\n", addr, value);
    break;
  case MMIO_WRITE:
    F_printf("MMIO: Writing word at %X: %04X\n", addr, value);
    break;
  default:
    F_printf("MMIO: Failed. Wrong arguments.");
  }
  return value;
}

uint32_t mmio_trace_dword(dosaddr_t addr, uint32_t value, uint8_t type)
{
  switch(type)
  {
  case MMIO_READ:
    F_printf("MMIO: Reading dword at %X: %08X\n", addr, value);
    break;
  case MMIO_WRITE:
    F_printf("MMIO: Writing dword at %X: %08X\n", addr, value);
    break;
  default:
    F_printf("MMIO: Failed. Wrong arguments.");
  }
  return value;
}

uint64_t mmio_trace_qword(dosaddr_t addr, uint64_t value, uint8_t type)
{
  switch(type)
  {
  case MMIO_READ:
    F_printf("MMIO: Reading qword at %X: %016lX\n", addr, value);
    break;
  case MMIO_WRITE:
    F_printf("MMIO: Writing qword at %X: %016lX\n", addr, value);
    break;
  default:
    F_printf("MMIO: Failed. Wrong arguments.");
  }
  return value;
}

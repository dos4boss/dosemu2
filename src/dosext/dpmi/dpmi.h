/* 
 * (C) Copyright 1992, ..., 2004 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

/* this is for the DPMI support */
#ifndef DPMI_H
#define DPMI_H

#include "emm.h"

#define DPMI_VERSION   		0x00	/* major version 0 */
#define DPMI_DRIVER_VERSION	0x5a	/* minor version 0.90 */

#define DPMI_MAX_CLIENTS	32	/* maximal number of clients */

#define DPMI_page_size		4096	/* 4096 bytes per page */

#define DPMI_pm_stack_size	0xf000	/* locked protected mode stack for exceptions, */
					/* hardware interrupts, software interrups 0x1c, */
					/* 0x23, 0x24 and real mode callbacks */

#define DPMI_rm_stacks		6	/* RM stacks per DPMI client */
#define DPMI_max_rec_rm_func	(DPMI_MAX_CLIENTS * DPMI_rm_stacks)	/* max number of recursive real mode functions */
#define DPMI_rm_stack_size	0x0200	/* real mode stack size */

#define DPMI_private_paragraphs	((DPMI_rm_stacks * DPMI_rm_stack_size)>>4)
					/* private data for DPMI server */
#define DTA_Para_ADD DPMI_private_paragraphs
#define DTA_Para_SIZE 8
#define RM_CB_Para_ADD (DTA_Para_ADD+DTA_Para_SIZE)
#define RM_CB_Para_SIZE 1
#define current_client (in_dpmi-1)
#define DPMI_CLIENT (DPMIclient[current_client])
#define PREV_DPMI_CLIENT (DPMIclient[current_client-1])

#define D_16_32(reg)		(DPMI_CLIENT.is_32 ? reg : reg & 0xffff)
#define ADD_16_32(acc, val)	{ if (DPMI_CLIENT.is_32) acc+=val; else LO_WORD(acc)+=val; }

#define PAGE_MASK	(~(PAGE_SIZE-1))
/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

/* Aargh!! Is this the only way we have to know if a signal interrupted
 * us in DPMI server or client code? */
#ifdef __linux__
#define UCODESEL _emu_stack_frame.cs
#define UDATASEL _emu_stack_frame.ds
#endif

#ifdef __linux__
int modify_ldt(int func, void *ptr, unsigned long bytecount);
#define LDT_WRITE 0x11
#endif
void direct_ldt_write(int offset, int length, char *buffer);

/* this is used like: SEL_ADR(_ss, _esp) */
#define SEL_ADR(seg, reg) \
({ unsigned long __res; \
  if (!((seg) & 0x0004)) { \
    /* GDT */ \
    __res = (unsigned long) reg; \
  } else { \
    /* LDT */ \
    if (Segments[seg>>3].is_32) \
      __res = (unsigned long) (GetSegmentBaseAddress(seg) + reg ); \
    else \
      __res = (unsigned long) (GetSegmentBaseAddress(seg) + *((unsigned short *)&(reg)) ); \
  } \
__res; })

#define HLT_OFF(addr) ((unsigned long)addr-(unsigned long)DPMI_dummy_start)

enum { es_INDEX, cs_INDEX, ss_INDEX, ds_INDEX, fs_INDEX, gs_INDEX,
  eax_INDEX, ebx_INDEX, ecx_INDEX, edx_INDEX, esi_INDEX, edi_INDEX,
  ebp_INDEX, esp_INDEX };

typedef struct pmaddr_s
{
    unsigned long	offset;
    unsigned short	selector, __selectorh;
} INTDESC;

typedef struct segment_descriptor_s
{
    unsigned long	base_addr;	/* Pointer to segment in flat memory */
    unsigned int	limit;		/* Limit of Segment */
    unsigned int	type:2;
    unsigned int	is_32:1;	/* one for is 32-bit Segment */
    unsigned int	readonly:1;	/* one for read only Segments */	
    unsigned int	is_big:1;	/* Granularity */
    unsigned int	not_present:1;		
    unsigned int	useable:1;		
    unsigned int	used;		/* Segment in use by client # */
} SEGDESC;

struct RealModeCallStructure {
  unsigned long edi;
  unsigned long esi;
  unsigned long ebp;
  unsigned long esp;
  unsigned long ebx;
  unsigned long edx;
  unsigned long ecx;
  unsigned long eax;
  unsigned short flags;
  unsigned short es;
  unsigned short ds;
  unsigned short fs;
  unsigned short gs;
  unsigned short ip;
  unsigned short cs;
  unsigned short sp;
  unsigned short ss;
};

typedef struct {
    unsigned short selector;
    unsigned long  offset;
    unsigned short rmreg_selector;
    unsigned long  rmreg_offset;
    struct RealModeCallStructure *rmreg;
    unsigned rm_ss_selector;
} RealModeCallBack;

typedef struct dpmi_pm_block_stuct {
  struct   dpmi_pm_block_stuct *next;
  unsigned long handle;
  unsigned long size;
  char     *base;
  u_short  *attrs;
  int linear;
} dpmi_pm_block;

typedef struct dpmi_pm_block_root_struc {
  dpmi_pm_block *first_pm_block;
} dpmi_pm_block_root;

struct DPMIclient_struct {
  struct sigcontext_struct stack_frame;
  int is_32;
  dpmi_pm_block_root *pm_block_root;
  unsigned short private_data_segment;
  int in_dpmi_rm_stack;
  /* for real mode call back, DPMI function 0x303 0x304 */
  RealModeCallBack realModeCallBack[0x10];
  INTDESC Interrupt_Table[0x100];
  INTDESC Exception_Table[0x20];
  unsigned short LDT_ALIAS;
  unsigned short PMSTACK_SEL;	/* protected mode stack selector */
  unsigned short DPMI_SEL;
  struct pmaddr_s mouseCallBack, PS2mouseCallBack; /* user\'s mouse routine */
  far_t XMS_call;
  /* used when passing a DTA higher than 1MB */
  unsigned short USER_DTA_SEL;
  unsigned long USER_DTA_OFF;
  unsigned short USER_PSP_SEL;
  unsigned short CURRENT_PSP;
  unsigned short CURRENT_ENV_SEL;
  char ems_map_buffer[PAGE_MAP_SIZE];
  int ems_frame_mapped;
};
extern struct DPMIclient_struct DPMIclient[DPMI_MAX_CLIENTS];

EXTERN volatile int in_dpmi INIT(0);/* Set to 1 when running under DPMI */
EXTERN volatile int in_win31 INIT(0); /* Set to 1 when running Windows 3.1 */
EXTERN volatile int in_dpmi_dos_int INIT(0);
EXTERN volatile int dpmi_mhp_TF INIT(0);
EXTERN unsigned char dpmi_mhp_intxxtab[256] INIT({0});
EXTERN volatile int is_cli INIT(0);

extern unsigned long dpmi_total_memory; /* total memory  of this session */
extern unsigned long dpmi_free_memory; /* how many bytes memory client */
				       /* can allocate */
extern unsigned long pm_block_handle_used;       /* tracking handle */
extern SEGDESC Segments[];
extern char *ldt_buffer;
extern char *pm_stack;

void dpmi_get_entry_point(void);
void indirect_dpmi_switch(struct sigcontext_struct *);
#ifdef __linux__
void dpmi_fault(struct sigcontext_struct *);
#endif
void dpmi_realmode_hlt(unsigned char *);
void run_pm_int(int);
void run_pm_dos_int(int);
void fake_pm_int(void);

#ifdef __linux__
int dpmi_mhp_regs(void);
void dpmi_mhp_getcseip(unsigned int *seg, unsigned int *off);
void dpmi_mhp_getssesp(unsigned int *seg, unsigned int *off);
int dpmi_mhp_get_selector_size(int sel);
int dpmi_mhp_getcsdefault(void);
int dpmi_mhp_setTF(int on);
void dpmi_mhp_GetDescriptor(unsigned short selector, unsigned long *lp);
int dpmi_mhp_getselbase(unsigned short selector);
unsigned long dpmi_mhp_getreg(int regnum);
void dpmi_mhp_setreg(int regnum, unsigned long val);
void dpmi_mhp_modify_eip(int delta);
#endif

void add_cli_to_blacklist(void);
dpmi_pm_block* DPMImalloc(unsigned long size);
dpmi_pm_block* DPMImallocLinear(unsigned long base, unsigned long size, int committed);
int DPMIfree(unsigned long handle);
dpmi_pm_block *DPMIrealloc(unsigned long handle, unsigned long size);
dpmi_pm_block *DPMIreallocLinear(unsigned long handle, unsigned long size,
  int committed);
void DPMIfreeAll(void);
unsigned long base2handle(void *);
dpmi_pm_block *lookup_pm_block(unsigned long h);
int
DPMIMapConventionalMemory(dpmi_pm_block *block, unsigned long offset,
			  unsigned long low_addr, unsigned long cnt);
int DPMISetPageAttributes(unsigned long handle, int offs, us attrs[], int count);
int DPMIGetPageAttributes(unsigned long handle, int offs, us attrs[], int count);
unsigned long dpmi_GetSegmentBaseAddress(unsigned short selector);
unsigned long GetSegmentBaseAddress(unsigned short);
unsigned long GetSegmentLimit(unsigned short);
int CheckSelectors(struct sigcontext_struct *scp, int in_dosemu);
int ValidSelector(unsigned short selector);
int ValidAndUsedSelector(unsigned short selector);

extern char *DPMI_show_state(struct sigcontext_struct *scp);
extern void dpmi_sigio(struct sigcontext_struct *scp);
extern void run_dpmi(void);

extern int ConvertSegmentToDescriptor(unsigned short segment);
extern int ConvertSegmentToCodeDescriptor(unsigned short segment);
extern int SetSegmentBaseAddress(unsigned short selector,
					unsigned long baseaddr);
extern int SetSegmentLimit(unsigned short, unsigned int);
extern void save_pm_regs(struct sigcontext_struct *);
extern void restore_pm_regs(struct sigcontext_struct *);
extern unsigned short AllocateDescriptors(int);
extern int SetSelector(unsigned short selector, unsigned long base_addr, unsigned int limit,
                       unsigned char is_32, unsigned char type, unsigned char readonly,
                       unsigned char is_big, unsigned char seg_not_present, unsigned char useable);
extern int FreeDescriptor(unsigned short selector);
extern void FreeSegRegs(struct sigcontext_struct *scp, unsigned short selector);
extern void dpmi_memory_init(void);
extern int lookup_realmode_callback(char *lina, int *num);
extern void dpmi_realmode_callback(int rmcb_client, int num);
extern int get_ldt(void *buffer);
void dpmi_return_request(void);
void dpmi_check_return(struct sigcontext_struct *scp);
void dpmi_init(void);

#endif /* DPMI_H */

/*
 * (C) Copyright 1992, ..., 2014 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

/* 	MS-DOS API translator for DOSEMU\'s DPMI Server
 *
 * DANG_BEGIN_MODULE msdos.c
 *
 * REMARK
 * MS-DOS API translator allows DPMI programs to call DOS service directly
 * in protected mode.
 *
 * /REMARK
 * DANG_END_MODULE
 *
 * First Attempted by Dong Liu,  dliu@rice.njit.edu
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#ifdef DJGPP_PORT
#include "wrapper.h"
#define SUPPORT_DOSEMU_HELPERS 0
#else
#include "emu.h"
#include "emu-ldt.h"
#include "bios.h"
#include "dpmi.h"
#include "dpmisel.h"
#include "hlt.h"
#include "utilities.h"
#include "dos2linux.h"
#define SUPPORT_DOSEMU_HELPERS 1
#endif
#include "emm.h"
#include "segreg.h"
#include "msdos.h"

#if SUPPORT_DOSEMU_HELPERS
#include "doshelpers.h"
#endif

#define TRANS_BUFFER_SEG EMM_SEGMENT
#define EXEC_SEG (MSDOS_CLIENT.lowmem_seg + EXEC_Para_ADD)

#define DTA_over_1MB (SEL_ADR(MSDOS_CLIENT.user_dta_sel, MSDOS_CLIENT.user_dta_off))
#define DTA_under_1MB SEGOFF2LINEAR(MSDOS_CLIENT.lowmem_seg + DTA_Para_ADD, 0)

#define MAX_DOS_PATH 260

#define D_16_32(reg)		(MSDOS_CLIENT.is_32 ? reg : reg & 0xffff)
#define MSDOS_CLIENT (msdos_client[msdos_client_num - 1])
#define CURRENT_ENV_SEL ((u_short)READ_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0x2c)))
#define WRITE_ENV_SEL(sel) WRITE_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0x2c), sel)

#define RMREG(x) rmreg->x
#define RMLWORD(x) LO_WORD(rmreg->x)
#define RM_LO(x) LO_BYTE(rmreg->e##x)
#define RMSEG_ADR(type, seg, reg)  type(&mem_base[(rmreg->seg << 4) + \
    (rmreg->e##reg & 0xffff)])

static int msdos_client_num = 0;
static struct msdos_struct msdos_client[DPMI_MAX_CLIENTS];

static int ems_frame_mapped;
static void *ems_map_buffer = NULL;
static u_short ems_frame_unmap[EMM_UMA_STD_PHYS * 2];
static u_short ems_frame_segs[EMM_UMA_STD_PHYS + 1];
static void lr_hlt(Bit32u idx, void *arg);
static void lw_hlt(Bit32u idx, void *arg);
static char *io_buffer;
static int io_buffer_size;

/* stack for AX values, needed for exec that can corrupt pm regs */
#define V_STK_LEN 16
static u_short v_stk[V_STK_LEN];
static int v_num;

static void push_v(u_short v)
{
    assert(v_num < V_STK_LEN);
    v_stk[v_num++] = v;
}

static u_short pop_v(void)
{
    assert(v_num > 0);
    return v_stk[--v_num];
}

void msdos_setup(void)
{
#define MK_LR_OFS(ofs) ((long)(ofs)-(long)MSDOS_lr_start)
#define MK_LW_OFS(ofs) ((long)(ofs)-(long)MSDOS_lw_start)
    int i;
    emu_hlt_t hlt_hdlr = HLT_INITIALIZER;
    u_short lr_off, lw_off;

    ems_map_buffer =
	malloc(emm_get_size_for_partial_page_map(EMM_UMA_STD_PHYS));
    ems_frame_segs[0] = EMM_UMA_STD_PHYS;
    for (i = 0; i < EMM_UMA_STD_PHYS; i++) {
	ems_frame_segs[i + 1] =
	    EMM_SEGMENT + i * (0x1000 / EMM_UMA_STD_PHYS);
	ems_frame_unmap[i * 2] = 0xffff;
	ems_frame_unmap[i * 2 + 1] = i;
    }

    hlt_hdlr.name = "msdos longread";
    hlt_hdlr.func = lr_hlt;
    lr_off = hlt_register_handler(hlt_hdlr);
    hlt_hdlr.name = "msdos longwrite";
    hlt_hdlr.func = lw_hlt;
    lw_off = hlt_register_handler(hlt_hdlr);

    WRITE_WORD(SEGOFF2LINEAR(DOS_LONG_READ_SEG, DOS_LONG_READ_OFF +
			     MK_LR_OFS(MSDOS_lr_entry_ip)), lr_off);
    WRITE_WORD(SEGOFF2LINEAR(DOS_LONG_READ_SEG, DOS_LONG_READ_OFF +
			     MK_LR_OFS(MSDOS_lr_entry_cs)),
	       BIOS_HLT_BLK_SEG);
    WRITE_WORD(SEGOFF2LINEAR
	       (DOS_LONG_WRITE_SEG,
		DOS_LONG_WRITE_OFF + MK_LW_OFS(MSDOS_lw_entry_ip)),
	       lw_off);
    WRITE_WORD(SEGOFF2LINEAR
	       (DOS_LONG_WRITE_SEG,
		DOS_LONG_WRITE_OFF + MK_LW_OFS(MSDOS_lw_entry_cs)),
	       BIOS_HLT_BLK_SEG);
}

void msdos_init(int is_32, unsigned short mseg)
{
    unsigned short envp;
    msdos_client_num++;
    memset(&MSDOS_CLIENT, 0, sizeof(struct msdos_struct));
    MSDOS_CLIENT.is_32 = is_32;
    MSDOS_CLIENT.lowmem_seg = mseg;
    /* convert environment pointer to a descriptor */
    envp = READ_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0x2c));
    if (envp) {
	WRITE_ENV_SEL(ConvertSegmentToDescriptor(envp));
	D_printf("DPMI: env segment %#x converted to descriptor %#x\n",
		 envp, CURRENT_ENV_SEL);
    }
    D_printf("MSDOS: init, %i\n", msdos_client_num);
}

void msdos_done(void)
{
    if (CURRENT_ENV_SEL)
	WRITE_ENV_SEL(GetSegmentBase(CURRENT_ENV_SEL) >> 4);
    msdos_client_num--;
    D_printf("MSDOS: done, %i\n", msdos_client_num);
}

int msdos_get_lowmem_size(void)
{
    return DTA_Para_SIZE + EXEC_Para_SIZE;
}

static unsigned int msdos_malloc(unsigned long size)
{
    int i;
    dpmi_pm_block block = DPMImalloc(size);
    if (!block.size)
	return 0;
    for (i = 0; i < MSDOS_MAX_MEM_ALLOCS; i++) {
	if (MSDOS_CLIENT.mem_map[i].size == 0) {
	    MSDOS_CLIENT.mem_map[i] = block;
	    break;
	}
    }
    return block.base;
}

static int msdos_free(unsigned int addr)
{
    int i;
    for (i = 0; i < MSDOS_MAX_MEM_ALLOCS; i++) {
	if (MSDOS_CLIENT.mem_map[i].base == addr) {
	    DPMIfree(MSDOS_CLIENT.mem_map[i].handle);
	    MSDOS_CLIENT.mem_map[i].size = 0;
	    return 0;
	}
    }
    return -1;
}

static unsigned int msdos_realloc(unsigned int addr, unsigned int new_size)
{
    int i;
    dpmi_pm_block block;
    block.size = 0;
    for (i = 0; i < MSDOS_MAX_MEM_ALLOCS; i++) {
	if (MSDOS_CLIENT.mem_map[i].base == addr) {
	    block = MSDOS_CLIENT.mem_map[i];
	    break;
	}
    }
    if (!block.size)
	return 0;
    block = DPMIrealloc(block.handle, new_size);
    if (!block.size)
	return 0;
    MSDOS_CLIENT.mem_map[i] = block;
    return block.base;
}

static void prepare_ems_frame(void)
{
    if (ems_frame_mapped)
	return;
    ems_frame_mapped = 1;
    emm_get_partial_map_registers(ems_map_buffer, ems_frame_segs);
    /* 0 is the special OS_HANDLE */
    emm_map_unmap_multi(ems_frame_unmap, 0, EMM_UMA_STD_PHYS);
}

static void restore_ems_frame(void)
{
    if (!ems_frame_mapped)
	return;
    emm_set_partial_map_registers(ems_map_buffer);
    ems_frame_mapped = 0;
}

static void get_ext_API(struct sigcontext_struct *scp)
{
    char *ptr = SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32);
    D_printf("MSDOS: GetVendorAPIEntryPoint: %s\n", ptr);
    if ((!strcmp("WINOS2", ptr)) || (!strcmp("MS-DOS", ptr))) {
	_LO(ax) = 0;
	_es = dpmi_sel();
	_edi = DPMI_SEL_OFF(MSDOS_API_call);
	_eflags &= ~CF;
    } else {
	_eflags |= CF;
    }
}

static int need_copy_dseg(int intr, u_short ax)
{
    switch (intr) {
    case 0x21:
	switch (HI_BYTE(ax)) {
	case 0x0a:		/* buffered keyboard input */
	case 0x5a:		/* mktemp */
	case 0x69:
	    return 1;
	case 0x44:		/* IOCTL */
	    switch (LO_BYTE(ax)) {
	    case 0x02 ... 0x05:
	    case 0x0c:
	    case 0x0d:
		return 1;
	    }
	    break;
	case 0x5d:		/* Critical Error Information  */
	    return (LO_BYTE(ax) != 0x06 && LO_BYTE(ax) != 0x0b);
	case 0x5e:
	    return (LO_BYTE(ax) != 0x03);
	}
	break;
    case 0x25:			/* Absolute Disk Read */
    case 0x26:			/* Absolute Disk write */
	return 1;
    }

    return 0;
}

static int need_copy_eseg(int intr, u_short ax)
{
    switch (intr) {
    case 0x10:			/* video */
	switch (HI_BYTE(ax)) {
	case 0x10:		/* Set/Get Palette Registers (EGA/VGA) */
	    switch (LO_BYTE(ax)) {
	    case 0x2:		/* set all palette registers and border */
	    case 0x09:		/* ead palette registers and border (PS/2) */
	    case 0x12:		/* set block of DAC color registers */
	    case 0x17:		/* read block of DAC color registers */
		return 1;
	    }
	    break;
	case 0x11:		/* Character Generator Routine (EGA/VGA) */
	    switch (LO_BYTE(ax)) {
	    case 0x0:		/* user character load */
	    case 0x10:		/* user specified character definition table */
	    case 0x20:
	    case 0x21:
		return 1;
	    }
	    break;
	case 0x13:		/* Write String */
	case 0x15:		/*  Return Physical Display Parms */
	case 0x1b:
	    return 1;
	case 0x1c:
	    if (LO_BYTE(ax) == 1 || LO_BYTE(ax) == 2)
		return 1;
	    break;
	}
	break;
    case 0x21:
	switch (HI_BYTE(ax)) {
	case 0x57:		/* Get/Set File Date and Time Using Handle */
	    if ((LO_BYTE(ax) == 0) || (LO_BYTE(ax) == 1)) {
		return 0;
	    }
	    return 1;
	case 0x5e:
	    return (LO_BYTE(ax) == 0x03);
	}
	break;
    case 0x33:
	switch (HI_BYTE(ax)) {
	case 0x16:		/* save state */
	case 0x17:		/* restore */
	    return 1;
	}
	break;
#if SUPPORT_DOSEMU_HELPERS
    case DOS_HELPER_INT:	/* dosemu helpers */
	switch (LO_BYTE(ax)) {
	case DOS_HELPER_PRINT_STRING:	/* print string to dosemu log */
	    return 1;
	}
	break;
#endif
    }

    return 0;
}

/* DOS selector is a selector whose base address is less than 0xffff0 */
/* and para. aligned.                                                 */
static int in_dos_space(unsigned short sel, unsigned long off)
{
    unsigned int base = GetSegmentBase(sel);

    if (base + off > 0x10ffef) {	/* ffff:ffff for DOS high */
	D_printf("MSDOS: base address %#x of sel %#x > DOS limit\n", base,
		 sel);
	return 0;
    } else if (base & 0xf) {
	D_printf("MSDOS: base address %#x of sel %#x not para. aligned.\n",
		 base, sel);
	return 0;
    } else
	return 1;
}

static void do_retf(void)
{
  unsigned int ssp, sp;

  ssp = SEGOFF2LINEAR(REG(ss), 0);
  sp = LWORD(esp);

  _IP = popw(ssp, sp);
  _CS = popw(ssp, sp);
  _SP += 4/* + 2 * pop_count*/;
}

static void do_call(int cs, int ip, struct RealModeCallStructure *rmreg)
{
  unsigned int ssp, sp;

  ssp = SEGOFF2LINEAR(RMREG(ss), 0);
  sp = RMREG(sp);

  g_printf("fake_call() CS:IP %04x:%04x\n", cs, ip);
  pushw(ssp, sp, cs);
  pushw(ssp, sp, ip);
  RMREG(sp) -= 4;
}

static void do_call_to(int cs, int ip, struct RealModeCallStructure *rmreg)
{
  do_call(RMREG(cs), RMREG(ip), rmreg);
  RMREG(cs) = cs;
  RMREG(ip) = ip;
}

static void old_dos_terminate(struct sigcontext_struct *scp, int i,
			      struct RealModeCallStructure *rmreg)
{
    unsigned short psp_seg_sel, parent_psp = 0;
    unsigned short psp_sig;

    D_printf("MSDOS: old_dos_terminate, int=%#x\n", i);

#if 0
    _eip = READ_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0xa));
    _cs =
	ConvertSegmentToCodeDescriptor(READ_WORD
				       (SEGOFF2LINEAR
					(CURRENT_PSP, 0xa + 2)));
#endif

    /* put our return address there */
    WRITE_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0xa), RMREG(ip));
    WRITE_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0xa + 2), RMREG(cs));
    /* cs should point to PSP, ip doesn't matter */
    RMREG(cs) = CURRENT_PSP;
    RMREG(ip) = 0x100;

    psp_seg_sel = READ_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0x16));
    /* try segment */
    psp_sig = READ_WORD(SEGOFF2LINEAR(psp_seg_sel, 0));
    if (psp_sig != 0x20CD) {
	/* now try selector */
	unsigned int addr;
	D_printf("MSDOS: Trying PSP sel=%#x, V=%i, d=%i, l=%#x\n",
		 psp_seg_sel, ValidAndUsedSelector(psp_seg_sel),
		 in_dos_space(psp_seg_sel, 0),
		 GetSegmentLimit(psp_seg_sel));
	if (ValidAndUsedSelector(psp_seg_sel)
	    && in_dos_space(psp_seg_sel, 0)
	    && GetSegmentLimit(psp_seg_sel) >= 0xff) {
	    addr = GetSegmentBase(psp_seg_sel);
	    psp_sig = READ_WORD(addr);
	    D_printf("MSDOS: Trying PSP sel=%#x, addr=%#x\n", psp_seg_sel,
		     addr);
	    if (!(addr & 0x0f) && psp_sig == 0x20CD) {
		/* found selector */
		parent_psp = addr >> 4;
		D_printf("MSDOS: parent PSP sel=%#x, seg=%#x\n",
			 psp_seg_sel, parent_psp);
	    }
	}
    } else {
	/* found segment */
	parent_psp = psp_seg_sel;
    }
    if (!parent_psp) {
	/* no PSP found, use current as the last resort */
	D_printf("MSDOS: using current PSP as parent!\n");
	parent_psp = CURRENT_PSP;
    }

    D_printf("MSDOS: parent PSP seg=%#x\n", parent_psp);
    if (parent_psp != psp_seg_sel)
	WRITE_WORD(SEGOFF2LINEAR(CURRENT_PSP, 0x16), parent_psp);
}

/*
 * DANG_BEGIN_FUNCTION msdos_pre_extender
 *
 * This function is called before a protected mode client goes to real
 * mode for DOS service. All protected mode selector is changed to
 * real mode segment register. And if client\'s data buffer is above 1MB,
 * necessary buffer copying is performed. This function returns 1 if
 * it does not need to go to real mode, otherwise returns 0.
 *
 * DANG_END_FUNCTION
 */

static int _msdos_pre_extender(struct sigcontext_struct *scp, int intr,
			       struct RealModeCallStructure *rmreg,
			       int *r_mask)
{
    int rm_mask = 0;
#define RMPRESERVE1(rg) (rm_mask |= (1 << rg##_INDEX))
#define RMPRESERVE2(rg1, rg2) (rm_mask |= ((1 << rg1##_INDEX) | (1 << rg2##_INDEX)))
#define SET_RMREG(rg, val) (RMPRESERVE1(rg), RMREG(rg) = (val))
#define ip_INDEX eip_INDEX
#define SET_RMLWORD(rg, val) SET_RMREG(rg, val)

    D_printf("MSDOS: pre_extender: int 0x%x, ax=0x%x\n", intr,
	     _LWORD(eax));
    if (MSDOS_CLIENT.user_dta_sel && intr == 0x21) {
	switch (_HI(ax)) {	/* functions use DTA */
	case 0x11:
	case 0x12:		/* find first/next using FCB */
	case 0x4e:
	case 0x4f:		/* find first/next */
	    MEMCPY_2DOS(DTA_under_1MB, DTA_over_1MB, 0x80);
	    break;
	}
    }

    /* only consider DOS and some BIOS services */
    switch (intr) {
    case 0x41:			/* win debug */
	return MSDOS_DONE;

    case 0x15:			/* misc */
	switch (_HI(ax)) {
	case 0xc2:
	    D_printf("MSDOS: PS2MOUSE function 0x%x\n", _LO(ax));
	    switch (_LO(ax)) {
	    case 0x07:		/* set handler addr */
		if (_es && D_16_32(_ebx)) {
		    D_printf
			("MSDOS: PS2MOUSE: set handler addr 0x%x:0x%x\n",
			 _es, D_16_32(_ebx));
		    MSDOS_CLIENT.PS2mouseCallBack.selector = _es;
		    MSDOS_CLIENT.PS2mouseCallBack.offset = D_16_32(_ebx);
		    SET_RMREG(es, DPMI_SEG);
		    SET_RMREG(ebx,
			DPMI_OFF + HLT_OFF(MSDOS_PS2_mouse_callback));
		} else {
		    D_printf("MSDOS: PS2MOUSE: reset handler addr\n");
		    SET_RMREG(es, 0);
		    SET_RMREG(ebx, 0);
		}
		break;
	    default:
		break;
	    }
	    break;
	default:
	    break;
	}
	break;
    case 0x20:			/* DOS terminate */
	old_dos_terminate(scp, intr, rmreg);
	RMPRESERVE2(cs, ip);
	break;
    case 0x21:
	switch (_HI(ax)) {
	    /* first see if we don\'t need to go to real mode */
	case 0x25:{		/* set vector */
		DPMI_INTDESC desc;
		desc.selector = _ds;
		desc.offset32 = D_16_32(_edx);
		dpmi_set_interrupt_vector(_LO(ax), desc);
		D_printf("MSDOS: int 21,ax=0x%04x, ds=0x%04x. dx=0x%04x\n",
			 _LWORD(eax), _ds, _LWORD(edx));
	    }
	    return MSDOS_DONE;
	case 0x35:{		/* Get Interrupt Vector */
		DPMI_INTDESC desc = dpmi_get_interrupt_vector(_LO(ax));
		_es = desc.selector;
		_ebx = desc.offset32;
		D_printf("MSDOS: int 21,ax=0x%04x, es=0x%04x. bx=0x%04x\n",
			 _LWORD(eax), _es, _LWORD(ebx));
	    }
	    return MSDOS_DONE;
	case 0x48:		/* allocate memory */
	    {
		unsigned long size = _LWORD(ebx) << 4;
		unsigned int addr = msdos_malloc(size);
		if (!addr) {
		    unsigned int meminfo[12];
		    GetFreeMemoryInformation(meminfo);
		    _eflags |= CF;
		    _LWORD(ebx) = meminfo[0] >> 4;
		    _LWORD(eax) = 0x08;
		} else {
		    unsigned short sel = AllocateDescriptors(1);
		    SetSegmentBaseAddress(sel, addr);
		    SetSegmentLimit(sel, size - 1);
		    _LWORD(eax) = sel;
		    _eflags &= ~CF;
		}
		return MSDOS_DONE;
	    }
	case 0x49:		/* free memory */
	    {
		if (msdos_free(GetSegmentBase(_es)) == -1) {
		    _eflags |= CF;
		} else {
		    _eflags &= ~CF;
		    FreeDescriptor(_es);
		    FreeSegRegs(scp, _es);
		}
		return MSDOS_DONE;
	    }
	case 0x4a:		/* reallocate memory */
	    {
		unsigned new_size = _LWORD(ebx) << 4;
		unsigned addr =
		    msdos_realloc(GetSegmentBase(_es), new_size);
		if (!addr) {
		    unsigned int meminfo[12];
		    GetFreeMemoryInformation(meminfo);
		    _eflags |= CF;
		    _LWORD(ebx) = meminfo[0] >> 4;
		    _LWORD(eax) = 0x08;
		} else {
		    SetSegmentBaseAddress(_es, addr);
		    SetSegmentLimit(_es, new_size - 1);
		    _eflags &= ~CF;
		}
		return MSDOS_DONE;
	    }
	case 0x01 ... 0x08:	/* These are dos functions which */
	case 0x0b ... 0x0e:	/* are not required memory copy, */
	case 0x19:		/* and segment register translation. */
	case 0x2a ... 0x2e:
	case 0x30 ... 0x34:
	case 0x36:
	case 0x37:
	case 0x3e:
	case 0x42:
	case 0x45:
	case 0x46:
	case 0x4d:
	case 0x4f:		/* find next */
	case 0x54:
	case 0x58:
	case 0x59:
	case 0x5c:		/* lock */
	case 0x66 ... 0x68:
	case 0xF8:		/* OEM SET vector */
	    break;
	case 0x00:		/* DOS terminate */
	    old_dos_terminate(scp, intr, rmreg);
	    RMPRESERVE2(cs, ip);
	    SET_RMLWORD(eax, 0x4c00);
	    break;
	case 0x09:		/* Print String */
	    {
		int i;
		char *s, *d;
		prepare_ems_frame();
		SET_RMREG(ds, TRANS_BUFFER_SEG);
		SET_RMREG(edx, 0);
		d = SEG2LINEAR(RMREG(ds));
		s = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
		for (i = 0; i < 0xffff; i++, d++, s++) {
		    *d = *s;
		    if (*s == '$')
			break;
		}
	    }
	    break;
	case 0x1a:		/* set DTA */
	    {
		unsigned long off = D_16_32(_edx);
		if (!in_dos_space(_ds, off)) {
		    MSDOS_CLIENT.user_dta_sel = _ds;
		    MSDOS_CLIENT.user_dta_off = off;
		    SET_RMREG(ds, MSDOS_CLIENT.lowmem_seg + DTA_Para_ADD);
		    SET_RMREG(edx, 0);
		    MEMCPY_2DOS(DTA_under_1MB, DTA_over_1MB, 0x80);
		} else {
		    SET_RMREG(ds, GetSegmentBase(_ds) >> 4);
		    MSDOS_CLIENT.user_dta_sel = 0;
		}
	    }
	    break;

	    /* FCB functions */
	case 0x0f:
	case 0x10:		/* These are not supported by */
	case 0x14:
	case 0x15:		/* dosx.exe, according to Ralf Brown */
	case 0x21 ... 0x24:
	case 0x27:
	case 0x28:
	    error("MS-DOS: Unsupported function 0x%x\n", _HI(ax));
	    _HI(ax) = 0xff;
	    return MSDOS_DONE;
	case 0x11:
	case 0x12:		/* find first/next using FCB */
	case 0x13:		/* Delete using FCB */
	case 0x16:		/* Create usring FCB */
	case 0x17:		/* rename using FCB */
	    prepare_ems_frame();
	    SET_RMREG(ds, TRANS_BUFFER_SEG);
	    SET_RMREG(edx, 0);
	    MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(ds), RMLWORD(edx)),
			SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32), 0x50);
	    break;
	case 0x29:		/* Parse a file name for FCB */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		SET_RMREG(ds, seg);
		SET_RMREG(esi, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(ds), RMLWORD(esi)),
			    SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32),
			    0x100);
		seg += 0x10;
		SET_RMREG(es, seg);
		SET_RMREG(edi, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)),
			    SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			    0x50);
	    }
	    break;
	case 0x47:		/* GET CWD */
	    prepare_ems_frame();
	    SET_RMREG(ds, TRANS_BUFFER_SEG);
	    SET_RMREG(esi, 0);
	    break;
	case 0x4b:		/* EXEC */
	    {
		/* we must copy all data from higher 1MB to lower 1MB */
		unsigned short segment = EXEC_SEG;
		char *p;
		unsigned short sel, off;

		D_printf("BCC: call dos exec\n");
		/* must copy command line */
		SET_RMREG(ds, segment);
		SET_RMREG(edx, 0);
		p = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
		snprintf((char *) SEG2LINEAR(RMREG(ds)), MAX_DOS_PATH,
			 "%s", p);
		segment += (MAX_DOS_PATH + 0x0f) >> 4;

		/* must copy parameter block */
		SET_RMREG(es, segment);
		SET_RMREG(ebx, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx)),
			    SEL_ADR_CLNT(_es, _ebx, MSDOS_CLIENT.is_32),
			    0x20);
		segment += 2;
#if 0
		/* now the envrionment segment */
		sel = READ_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx)));
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx)),
			   segment);
		MEMCPY_2DOS(SEGOFF2LINEAR(segment, 0),	/* 4K envr. */
			    GetSegmentBase(sel), 0x1000);
		segment += 0x100;
#else
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx)), 0);
#endif
		/* now the tail of the command line */
		off =
		    READ_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 2));
		sel =
		    READ_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 4));
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 4),
			   segment);
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 2), 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(segment, 0),
			    SEL_ADR_CLNT(sel, off, MSDOS_CLIENT.is_32),
			    0x80);
		segment += 8;

		/* set the FCB pointers to something reasonable */
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 6), 0);
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 8),
			   segment);
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 0xA),
			   0);
		WRITE_WORD(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx) + 0xC),
			   segment);
		MEMSET_DOS(SEGOFF2LINEAR(segment, 0), 0, 0x30);
		segment += 3;

		/* then the enviroment seg */
		if (CURRENT_ENV_SEL)
		    WRITE_ENV_SEL(GetSegmentBase(CURRENT_ENV_SEL) >> 4);

		if (segment != EXEC_SEG + EXEC_Para_SIZE)
		    error("DPMI: exec: seg=%#x (%#x), size=%#x\n",
			  segment, segment - EXEC_SEG, EXEC_Para_SIZE);
		if (ems_frame_mapped)
		    error
			("DPMI: exec: EMS frame should not be mapped here\n");

		/* execed client can use raw mode switch and expects not to
		 * corrupt the registers of its parent, so we need to save
		 * them explicitly.
		 * Either that or create a new DPMI client here. */
		save_pm_regs(scp);
	    }
	    break;

	case 0x50:		/* set PSP */
	    if (!in_dos_space(_LWORD(ebx), 0)) {
		MSDOS_CLIENT.user_psp_sel = _LWORD(ebx);
		SET_RMLWORD(ebx, CURRENT_PSP);
		MEMCPY_DOS2DOS(SEGOFF2LINEAR(RMLWORD(ebx), 0),
			       GetSegmentBase(_LWORD(ebx)), 0x100);
		D_printf("MSDOS: PSP moved from %x to %x\n",
			 GetSegmentBase(_LWORD(ebx)),
			 SEGOFF2LINEAR(RMLWORD(ebx), 0));
	    } else {
		SET_RMREG(ebx, GetSegmentBase(_LWORD(ebx)) >> 4);
		MSDOS_CLIENT.user_psp_sel = 0;
	    }
	    break;

	case 0x26:		/* create PSP */
	    prepare_ems_frame();
	    SET_RMREG(edx, TRANS_BUFFER_SEG);
	    break;

	case 0x55:		/* create & set PSP */
	    if (!in_dos_space(_LWORD(edx), 0)) {
		MSDOS_CLIENT.user_psp_sel = _LWORD(edx);
		SET_RMLWORD(edx, CURRENT_PSP);
	    } else {
		SET_RMREG(edx, GetSegmentBase(_LWORD(edx)) >> 4);
		MSDOS_CLIENT.user_psp_sel = 0;
	    }
	    break;

	case 0x39:		/* mkdir */
	case 0x3a:		/* rmdir */
	case 0x3b:		/* chdir */
	case 0x3c:		/* creat */
	case 0x3d:		/* Dos OPEN */
	case 0x41:		/* unlink */
	case 0x43:		/* change attr */
	case 0x4e:		/* find first */
	case 0x5b:		/* Create */
	    if ((_HI(ax) == 0x4e) && (_ecx & 0x8))
		D_printf("MSDOS: MS-DOS try to find volume label\n");
	    {
		char *src, *dst;
		prepare_ems_frame();
		SET_RMREG(ds, TRANS_BUFFER_SEG);
		SET_RMREG(edx, 0);
		src = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
		dst = RMSEG_ADR((char *), ds, dx);
		D_printf("MSDOS: passing ASCIIZ > 1MB to dos %p\n", dst);
		D_printf("%p: '%s'\n", src, src);
		snprintf(dst, MAX_DOS_PATH, "%s", src);
	    }
	    break;
	case 0x38:
	    if (_LWORD(edx) != 0xffff) {	/* get country info */
		prepare_ems_frame();
		SET_RMREG(ds, TRANS_BUFFER_SEG);
		SET_RMREG(edx, 0);
	    }
	    break;
	case 0x3f:		/* dos read */
	    io_buffer = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
	    io_buffer_size = D_16_32(_ecx);
	    prepare_ems_frame();
	    SET_RMREG(ds, TRANS_BUFFER_SEG);
	    SET_RMREG(edx, 0);
	    SET_RMREG(ecx, D_16_32(_ecx));
	    do_call_to(DOS_LONG_READ_SEG, DOS_LONG_READ_OFF, rmreg);
	    return MSDOS_ALT_ENT;
	case 0x40:		/* DOS Write */
	    io_buffer = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
	    io_buffer_size = D_16_32(_ecx);
	    prepare_ems_frame();
	    SET_RMREG(ds, TRANS_BUFFER_SEG);
	    SET_RMREG(edx, 0);
	    SET_RMREG(ecx, D_16_32(_ecx));
	    do_call_to(DOS_LONG_WRITE_SEG, DOS_LONG_WRITE_OFF, rmreg);
	    return MSDOS_ALT_ENT;
	case 0x53:		/* Generate Drive Parameter Table  */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		SET_RMREG(ds, seg);
		SET_RMREG(esi, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(ds), RMLWORD(esi)),
			    SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32),
			    0x30);
		seg += 3;

		SET_RMREG(es, seg);
		SET_RMREG(ebp, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(es), RMLWORD(ebp)),
			    SEL_ADR_CLNT(_es, _ebp, MSDOS_CLIENT.is_32),
			    0x60);
	    }
	    break;
	case 0x56:		/* rename file */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		SET_RMREG(ds, seg);
		SET_RMREG(edx, 0);
		snprintf(SEG2LINEAR(RMREG(ds)), MAX_DOS_PATH, "%s",
			 (char *) SEL_ADR_CLNT(_ds, _edx,
					       MSDOS_CLIENT.is_32));
		seg += 0x20;

		SET_RMREG(es, seg);
		SET_RMREG(edi, 0);
		snprintf(SEG2LINEAR(RMREG(es)), MAX_DOS_PATH, "%s",
			 (char *) SEL_ADR_CLNT(_es, _edi,
					       MSDOS_CLIENT.is_32));
	    }
	    break;
	case 0x5f:		/* redirection */
	    switch (_LO(ax)) {
		unsigned short seg;
	    case 0:
	    case 1:
		break;
	    case 2 ... 6:
		prepare_ems_frame();
		seg = TRANS_BUFFER_SEG;
		SET_RMREG(ds, seg);
		SET_RMREG(esi, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(ds), RMLWORD(esi)),
			    SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32),
			    0x100);
		seg += 0x10;
		SET_RMREG(es, seg);
		SET_RMREG(edi, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)),
			    SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			    0x100);
		break;
	    }
	case 0x60:		/* Get Fully Qualified File Name */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		SET_RMREG(ds, seg);
		SET_RMREG(esi, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(ds), RMLWORD(esi)),
			    SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32),
			    0x100);
		seg += 0x10;
		SET_RMREG(es, seg);
		SET_RMREG(edi, 0);
		break;
	    }
	case 0x6c:		/*  Extended Open/Create */
	    {
		char *src, *dst;
		prepare_ems_frame();
		SET_RMREG(ds, TRANS_BUFFER_SEG);
		SET_RMREG(esi, 0);
		src = SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32);
		dst = RMSEG_ADR((char *), ds, si);
		D_printf("MSDOS: passing ASCIIZ > 1MB to dos %p\n", dst);
		D_printf("%p: '%s'\n", src, src);
		snprintf(dst, MAX_DOS_PATH, "%s", src);
	    }
	    break;
	case 0x65:		/* internationalization */
	    switch (_LO(ax)) {
	    case 0:
		prepare_ems_frame();
		SET_RMREG(es, TRANS_BUFFER_SEG);
		SET_RMREG(edi, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)),
			    SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			    _LWORD(ecx));
		break;
	    case 1 ... 7:
		prepare_ems_frame();
		SET_RMREG(es, TRANS_BUFFER_SEG);
		SET_RMREG(edi, 0);
		break;
	    case 0x21:
	    case 0xa1:
		prepare_ems_frame();
		SET_RMREG(ds, TRANS_BUFFER_SEG);
		SET_RMREG(edx, 0);
		MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(ds), RMLWORD(edx)),
			    SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32),
			    _LWORD(ecx));
		break;
	    case 0x22:
	    case 0xa2:
		prepare_ems_frame();
		SET_RMREG(ds, TRANS_BUFFER_SEG);
		SET_RMREG(edx, 0);
		strcpy(RMSEG_ADR((void *), ds, dx),
		       SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32));
		break;
	    }
	    break;
	case 0x71:		/* LFN functions */
	    {
		char *src, *dst;
		switch (_LO(ax)) {
		case 0x3B:	/* change dir */
		case 0x41:	/* delete file */
		case 0x43:	/* get file attributes */
		    prepare_ems_frame();
		    SET_RMREG(ds, TRANS_BUFFER_SEG);
		    SET_RMREG(edx, 0);
		    src = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
		    dst = RMSEG_ADR((char *), ds, dx);
		    snprintf(dst, MAX_DOS_PATH, "%s", src);
		    break;
		case 0x4E:	/* find first file */
		    prepare_ems_frame();
		    SET_RMREG(ds, TRANS_BUFFER_SEG);
		    SET_RMREG(edx, 0);
		    SET_RMREG(es, TRANS_BUFFER_SEG);
		    SET_RMREG(edi, MAX_DOS_PATH);
		    src = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
		    dst = RMSEG_ADR((char *), ds, dx);
		    snprintf(dst, MAX_DOS_PATH, "%s", src);
		    break;
		case 0x4F:	/* find next file */
		    prepare_ems_frame();
		    SET_RMREG(es, TRANS_BUFFER_SEG);
		    SET_RMREG(edi, 0);
		    MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)),
				SEL_ADR_CLNT(_es, _edi,
					     MSDOS_CLIENT.is_32), 0x13e);
		    break;
		case 0x47:	/* get cur dir */
		    prepare_ems_frame();
		    SET_RMREG(ds, TRANS_BUFFER_SEG);
		    SET_RMREG(esi, 0);
		    break;
		case 0x60:	/* canonicalize filename */
		    prepare_ems_frame();
		    SET_RMREG(ds, TRANS_BUFFER_SEG);
		    SET_RMREG(esi, 0);
		    SET_RMREG(es, TRANS_BUFFER_SEG);
		    SET_RMREG(edi, MAX_DOS_PATH);
		    src = SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32);
		    dst = RMSEG_ADR((char *), ds, si);
		    snprintf(dst, MAX_DOS_PATH, "%s", src);
		    break;
		case 0x6c:	/* extended open/create */
		    prepare_ems_frame();
		    SET_RMREG(ds, TRANS_BUFFER_SEG);
		    SET_RMREG(esi, 0);
		    src = SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32);
		    dst = RMSEG_ADR((char *), ds, si);
		    snprintf(dst, MAX_DOS_PATH, "%s", src);
		    break;
		case 0xA0:	/* get volume info */
		    prepare_ems_frame();
		    SET_RMREG(ds, TRANS_BUFFER_SEG);
		    SET_RMREG(edx, 0);
		    SET_RMREG(es, TRANS_BUFFER_SEG);
		    SET_RMREG(edi, MAX_DOS_PATH);
		    src = SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32);
		    dst = RMSEG_ADR((char *), ds, dx);
		    snprintf(dst, MAX_DOS_PATH, "%s", src);
		    break;
		case 0xA1:	/* close find */
		    break;
		case 0xA6:	/* get file info by handle */
		    prepare_ems_frame();
		    SET_RMREG(ds, TRANS_BUFFER_SEG);
		    SET_RMREG(edx, 0);
		    break;
		default:	/* all other subfuntions currently not supported */
		    _eflags |= CF;
		    _eax = _eax & 0xFFFFFF00;
		    return 1;
		}
	    }
	default:
	    break;
	}
	break;
    case 0x2f:
	switch (_LWORD(eax)) {
	case 0x168a:
	    get_ext_API(scp);
	    return MSDOS_DONE;
	}
	break;
    case 0x33:			/* mouse */
	switch (_LWORD(eax)) {
	case 0x09:		/* Set Mouse Graphics Cursor */
	    prepare_ems_frame();
	    SET_RMREG(es, TRANS_BUFFER_SEG);
	    SET_RMREG(edx, 0);
	    MEMCPY_2DOS(SEGOFF2LINEAR(RMREG(es), RMLWORD(edx)),
			SEL_ADR_CLNT(_es, _edx, MSDOS_CLIENT.is_32), 16);
	    break;
	case 0x0c:		/* set call back */
	case 0x14:{		/* swap call back */
		struct pmaddr_s old_callback = MSDOS_CLIENT.mouseCallBack;
		MSDOS_CLIENT.mouseCallBack.selector = _es;
		MSDOS_CLIENT.mouseCallBack.offset = D_16_32(_edx);
		if (_es) {
		    D_printf("MSDOS: set mouse callback\n");
		    SET_RMREG(es, DPMI_SEG);
		    SET_RMREG(edx, DPMI_OFF + HLT_OFF(MSDOS_mouse_callback));
		} else {
		    D_printf("MSDOS: reset mouse callback\n");
		    SET_RMREG(es, 0);
		    SET_RMREG(edx, 0);
		}
		if (_LWORD(eax) == 0x14) {
		    _es = old_callback.selector;
		    if (MSDOS_CLIENT.is_32)
			_edx = old_callback.offset;
		    else
			_LWORD(edx) = old_callback.offset;
		}
	    }
	    break;
	}
	break;
    }

    if (need_copy_dseg(intr, _LWORD(eax))) {
	unsigned int src, dst;
	int len;
	prepare_ems_frame();
	SET_RMREG(ds, TRANS_BUFFER_SEG);
	src = GetSegmentBase(_ds);
	dst = SEGOFF2LINEAR(RMREG(ds), 0);
	len = min((int) (GetSegmentLimit(_ds) + 1), 0x10000);
	D_printf
	    ("MSDOS: whole segment of DS at %x copy to DOS at %x for %#x\n",
	     src, dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }

    if (need_copy_eseg(intr, _LWORD(eax))) {
	unsigned int src, dst;
	int len;
	prepare_ems_frame();
	SET_RMREG(es, TRANS_BUFFER_SEG);
	src = GetSegmentBase(_es);
	dst = SEGOFF2LINEAR(RMREG(es), 0);
	len = min((int) (GetSegmentLimit(_es) + 1), 0x10000);
	D_printf
	    ("MSDOS: whole segment of ES at %x copy to DOS at %x for %#x\n",
	     src, dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }

    *r_mask = rm_mask;
    return 0;
}

int msdos_pre_extender(struct sigcontext_struct *scp, int intr,
		       struct RealModeCallStructure *rmreg, int *r_mask)
{
    int ret = _msdos_pre_extender(scp, intr, rmreg, r_mask);
    if (!(ret & MSDOS_DONE))
	push_v(_LWORD(eax));
    return ret;
}

/*
 * DANG_BEGIN_FUNCTION msdos_post_extender
 *
 * This function is called after return from real mode DOS service
 * All real mode segment registers are changed to protected mode selectors
 * And if client\'s data buffer is above 1MB, necessary buffer copying
 * is performed.
 *
 * DANG_END_FUNCTION
 */

static int _msdos_post_extender(struct sigcontext_struct *scp, int intr,
				u_short ax,
				struct RealModeCallStructure *rmreg)
{
    int update_mask = ~0;
#define PRESERVE1(rg) (update_mask &= ~(1 << rg##_INDEX))
#define PRESERVE2(rg1, rg2) (update_mask &= ~((1 << rg1##_INDEX) | (1 << rg2##_INDEX)))
#define SET_REG(rg, val) (PRESERVE1(rg), _##rg = (val))
    D_printf("MSDOS: post_extender: int 0x%x ax=0x%04x\n", intr, ax);

    if (MSDOS_CLIENT.user_dta_sel && intr == 0x21) {
	switch (HI_BYTE(ax)) {	/* functions use DTA */
	case 0x11:
	case 0x12:		/* find first/next using FCB */
	case 0x4e:
	case 0x4f:		/* find first/next */
	    MEMCPY_2UNIX(DTA_over_1MB, DTA_under_1MB, 0x80);
	    break;
	}
    }

    if (need_copy_dseg(intr, ax)) {
	unsigned short my_ds;
	unsigned int src, dst;
	int len;
	my_ds = TRANS_BUFFER_SEG;
	src = SEGOFF2LINEAR(my_ds, 0);
	dst = GetSegmentBase(_ds);
	len = min((int) (GetSegmentLimit(_ds) + 1), 0x10000);
	D_printf("MSDOS: DS seg at %x copy back at %x for %#x\n",
		 src, dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }

    if (need_copy_eseg(intr, ax)) {
	unsigned short my_es;
	unsigned int src, dst;
	int len;
	my_es = TRANS_BUFFER_SEG;
	src = SEGOFF2LINEAR(my_es, 0);
	dst = GetSegmentBase(_es);
	len = min((int) (GetSegmentLimit(_es) + 1), 0x10000);
	D_printf("MSDOS: ES seg at %x copy back at %x for %#x\n",
		 src, dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }

    switch (intr) {
    case 0x10:			/* video */
	if (ax == 0x1130) {
	    /* get current character generator infor */
	    SET_REG(es, ConvertSegmentToDescriptor(RMREG(es)));
	}
	break;
    case 0x15:
	/* we need to save regs at int15 because AH has the return value */
	if (HI_BYTE(ax) == 0xc0) {	/* Get Configuration */
	    if (RMREG(flags) & CF)
		break;
	    SET_REG(es, ConvertSegmentToDescriptor(RMREG(es)));
	}
	break;
    case 0x2f:
	switch (ax) {
	case 0x4310:
	    MSDOS_CLIENT.XMS_call = MK_FARt(RMREG(es), RMLWORD(ebx));
	    SET_REG(es, dpmi_sel());
	    SET_REG(ebx, DPMI_SEL_OFF(MSDOS_XMS_call));
	    break;
	}
	break;

    case 0x21:
	switch (HI_BYTE(ax)) {
	case 0x00:		/* psp kill */
	    PRESERVE1(eax);
	    break;
	case 0x09:		/* print String */
	case 0x1a:		/* set DTA */
	    PRESERVE1(edx);
	    break;
	case 0x11:
	case 0x12:		/* findfirst/next using FCB */
	case 0x13:		/* Delete using FCB */
	case 0x16:		/* Create usring FCB */
	case 0x17:		/* rename using FCB */
	    PRESERVE1(edx);
	    MEMCPY_2UNIX(SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32),
			 SEGOFF2LINEAR(RMREG(ds), RMLWORD(edx)), 0x50);
	    break;

	case 0x29:		/* Parse a file name for FCB */
	    PRESERVE2(esi, edi);
	    MEMCPY_2UNIX(SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32),
			 /* Warning: SI altered, assume old value = 0, don't touch. */
			 SEGOFF2LINEAR(RMREG(ds), 0), 0x100);
	    SET_REG(esi, _esi + RMLWORD(esi));
	    MEMCPY_2UNIX(SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			 SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)), 0x50);
	    break;

	case 0x2f:		/* GET DTA */
	    if (SEGOFF2LINEAR(RMREG(es), RMLWORD(ebx)) == DTA_under_1MB) {
		if (!MSDOS_CLIENT.user_dta_sel)
		    error("Selector is not set for the translated DTA\n");
		SET_REG(es, MSDOS_CLIENT.user_dta_sel);
		SET_REG(ebx, MSDOS_CLIENT.user_dta_off);
	    } else {
		SET_REG(es, ConvertSegmentToDescriptor(RMREG(es)));
		/* it is important to copy only the lower word of ebx
		 * and make the higher word zero, so do it here instead
		 * of relying on dpmi.c */
		SET_REG(ebx, RMLWORD(ebx));
	    }
	    break;

	case 0x34:		/* Get Address of InDOS Flag */
	case 0x35:		/* GET Vector */
	case 0x52:		/* Get List of List */
	    SET_REG(es, ConvertSegmentToDescriptor(RMREG(es)));
	    break;

	case 0x39:		/* mkdir */
	case 0x3a:		/* rmdir */
	case 0x3b:		/* chdir */
	case 0x3c:		/* creat */
	case 0x3d:		/* Dos OPEN */
	case 0x41:		/* unlink */
	case 0x43:		/* change attr */
	case 0x4e:		/* find first */
	case 0x5b:		/* Create */
	    PRESERVE1(edx);
	    break;

	case 0x50:		/* Set PSP */
	    PRESERVE1(ebx);
	    break;

	case 0x6c:		/*  Extended Open/Create */
	    PRESERVE1(esi);
	    break;

	case 0x55:		/* create & set PSP */
	    PRESERVE1(edx);
	    if (!in_dos_space(_LWORD(edx), 0)) {
		MEMCPY_DOS2DOS(GetSegmentBase(_LWORD(edx)),
			       SEGOFF2LINEAR(RMLWORD(edx), 0), 0x100);
	    }
	    break;

	case 0x26:		/* create PSP */
	    PRESERVE1(edx);
	    MEMCPY_DOS2DOS(GetSegmentBase(_LWORD(edx)),
			   SEGOFF2LINEAR(RMLWORD(edx), 0), 0x100);
	    break;

	case 0x59:		/* Get EXTENDED ERROR INFORMATION */
	    if (RMLWORD(eax) == 0x22) {	/* only this code has a pointer */
		SET_REG(es, ConvertSegmentToDescriptor(RMREG(es)));
	    }
	    break;
	case 0x38:
	    if (_LWORD(edx) != 0xffff) {	/* get country info */
		PRESERVE1(edx);
		if (RMLWORD(flags) & CF)
		    break;
		/* FreeDOS copies only 0x18 bytes */
		MEMCPY_2UNIX(SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32),
			     SEGOFF2LINEAR(RMREG(ds), RMLWORD(edx)), 0x18);
	    }
	    break;
	case 0x47:		/* get CWD */
	    PRESERVE1(esi);
	    if (RMLWORD(flags) & CF)
		break;
	    snprintf(SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32), 0x40,
		     "%s", RMSEG_ADR((char *), ds, si));
	    D_printf("MSDOS: CWD: %s\n",
		     (char *) SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32));
	    break;
#if 0
	case 0x48:		/* allocate memory */
	    if (RMLWORD(eflags) & CF)
		break;
	    SET_REG(eax, ConvertSegmentToDescriptor(RMLWORD(eax)));
	    break;
#endif
	case 0x4b:		/* EXEC */
	    restore_pm_regs(scp);
	    if (CURRENT_ENV_SEL)
		WRITE_ENV_SEL(ConvertSegmentToDescriptor(CURRENT_ENV_SEL));
	    D_printf("DPMI: Return from DOS exec\n");
	    break;
	case 0x51:		/* get PSP */
	case 0x62:
	    {			/* convert environment pointer to a descriptor */
		unsigned short psp = RMLWORD(ebx);
		if (psp == CURRENT_PSP && MSDOS_CLIENT.user_psp_sel) {
		    SET_REG(ebx, MSDOS_CLIENT.user_psp_sel);
		} else {
		    SET_REG(ebx,
			    ConvertSegmentToDescriptor_lim(psp, 0xff));
		}
	    }
	    break;
	case 0x53:		/* Generate Drive Parameter Table  */
	    PRESERVE2(esi, ebp);
	    MEMCPY_2UNIX(SEL_ADR_CLNT(_es, _ebp, MSDOS_CLIENT.is_32),
			 SEGOFF2LINEAR(RMREG(es), RMLWORD(ebp)), 0x60);
	    break;
	case 0x56:		/* rename */
	    PRESERVE2(edx, edi);
	    break;
	case 0x5d:
	    if (_LO(ax) == 0x06 || _LO(ax) == 0x0b)
		/* get address of DOS swappable area */
		/*        -> DS:SI                     */
		SET_REG(ds, ConvertSegmentToDescriptor(RMREG(ds)));
	    break;
	case 0x63:		/* Get Lead Byte Table Address */
	    /* _LO(ax)==0 is to test for 6300 on input, RM_LO(ax)==0 for success */
	    if (_LO(ax) == 0 && RM_LO(ax) == 0)
		SET_REG(ds, ConvertSegmentToDescriptor(RMREG(ds)));
	    break;

	case 0x3f:
	    io_buffer_size = 0;
	    PRESERVE2(edx, ecx);
	    break;
	case 0x40:
	    io_buffer_size = 0;
	    PRESERVE2(edx, ecx);
	    break;
	case 0x5f:		/* redirection */
	    switch (_LO(ax)) {
	    case 0:
	    case 1:
		break;
	    case 2 ... 6:
		PRESERVE2(esi, edi);
		MEMCPY_2UNIX(SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32),
			     SEGOFF2LINEAR(RMREG(ds), RMLWORD(esi)),
			     0x100);
		MEMCPY_2UNIX(SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			     SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)),
			     0x100);
	    }
	    break;
	case 0x60:		/* Canonicalize file name */
	    PRESERVE2(esi, edi);
	    MEMCPY_2UNIX(SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			 SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)), 0x80);
	    break;
	case 0x65:		/* internationalization */
	    PRESERVE2(edi, edx);
	    if (RMLWORD(flags) & CF)
		break;
	    switch (_LO(ax)) {
	    case 1 ... 7:
		MEMCPY_2UNIX(SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			     SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)),
			     RMLWORD(ecx));
		break;
	    case 0x21:
	    case 0xa1:
		MEMCPY_2UNIX(SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32),
			     SEGOFF2LINEAR(RMREG(ds), RMLWORD(edx)),
			     _LWORD(ecx));
		break;
	    case 0x22:
	    case 0xa2:
		strcpy(SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32),
		       RMSEG_ADR((void *), ds, dx));
		break;
	    }
	    break;
	case 0x71:		/* LFN functions */
	    switch (_LO(ax)) {
	    case 0x3B:
	    case 0x41:
	    case 0x43:
		PRESERVE1(edx);
		break;
	    case 0x4E:
		PRESERVE1(edx);
		/* fall thru */
	    case 0x4F:
		PRESERVE1(edi);
		if (RMLWORD(flags) & CF)
		    break;
		MEMCPY_2UNIX(SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			     SEGOFF2LINEAR(RMREG(es), RMLWORD(edi)),
			     0x13E);
		break;
	    case 0x47:
		PRESERVE1(esi);
		if (RMLWORD(flags) & CF)
		    break;
		snprintf(SEL_ADR_CLNT(_ds, _esi, MSDOS_CLIENT.is_32),
			 MAX_DOS_PATH, "%s", RMSEG_ADR((char *), ds, si));
		break;
	    case 0x60:
		PRESERVE2(esi, edi);
		if (RMLWORD(flags) & CF)
		    break;
		snprintf(SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			 MAX_DOS_PATH, "%s", RMSEG_ADR((char *), es, di));
		break;
	    case 0x6c:
		PRESERVE1(esi);
		break;
	    case 0xA0:
		PRESERVE2(edx, edi);
		if (RMLWORD(flags) & CF)
		    break;
		snprintf(SEL_ADR_CLNT(_es, _edi, MSDOS_CLIENT.is_32),
			 _LWORD(ecx), "%s", RMSEG_ADR((char *), es, di));
		break;
	    case 0xA6:
		PRESERVE1(edx);
		if (RMLWORD(flags) & CF)
		    break;
		MEMCPY_2UNIX(SEL_ADR_CLNT(_ds, _edx, MSDOS_CLIENT.is_32),
			     SEGOFF2LINEAR(RMREG(ds), RMLWORD(edx)), 0x34);
		break;
	    };

	default:
	    break;
	}
	break;
    case 0x25:			/* Absolute Disk Read */
    case 0x26:			/* Absolute Disk Write */
	/* the flags should be pushed to stack */
	if (MSDOS_CLIENT.is_32) {
	    _esp -= 4;
	    *(uint32_t *) (SEL_ADR_CLNT(_ss, _esp - 4, MSDOS_CLIENT.is_32))
		= RMREG(flags);
	} else {
	    _esp -= 2;
	    *(uint16_t
	      *) (SEL_ADR_CLNT(_ss, _LWORD(esp) - 2, MSDOS_CLIENT.is_32)) =
       RMLWORD(flags);
	}
	break;
    case 0x33:			/* mouse */
	switch (ax) {
	case 0x09:		/* Set Mouse Graphics Cursor */
	case 0x14:		/* swap call back */
	    PRESERVE1(edx);
	    break;
	case 0x19:		/* Get User Alternate Interrupt Address */
	    SET_REG(ebx, ConvertSegmentToDescriptor(RMLWORD(ebx)));
	    break;
	default:
	    break;
	}
	break;
    }
    restore_ems_frame();
    return update_mask;
}

void msdos_post_extender(struct sigcontext_struct *scp, int intr,
			 struct RealModeCallStructure *rmreg)
{
    int ret = _msdos_post_extender(scp, intr, pop_v(), rmreg);
    rm_to_pm_regs(scp, ret);
}

int msdos_pre_rm(struct sigcontext_struct *scp,
		 const struct RealModeCallStructure *rmreg)
{
    unsigned int lina = SEGOFF2LINEAR(RMREG(cs), RMREG(ip)) - 1;
    void *sp = SEL_ADR_CLNT(_ss, _esp, MSDOS_CLIENT.is_32);

    if (lina == DPMI_ADD + HLT_OFF(MSDOS_mouse_callback)) {
	if (!ValidAndUsedSelector(MSDOS_CLIENT.mouseCallBack.selector)) {
	    D_printf("MSDOS: ERROR: mouse callback to unused segment\n");
	    return 0;
	}
	D_printf("MSDOS: starting mouse callback\n");
	rm_to_pm_regs(scp, ~0);
	_ds = ConvertSegmentToDescriptor(RMREG(ds));
	_cs = MSDOS_CLIENT.mouseCallBack.selector;
	_eip = MSDOS_CLIENT.mouseCallBack.offset;

	if (MSDOS_CLIENT.is_32) {
	    unsigned int *ssp = sp;
	    *--ssp = dpmi_sel();
	    *--ssp = DPMI_SEL_OFF(MSDOS_return_from_pm);
	    _esp -= 8;
	} else {
	    unsigned short *ssp = sp;
	    *--ssp = dpmi_sel();
	    *--ssp = DPMI_SEL_OFF(MSDOS_return_from_pm);
	    _LWORD(esp) -= 4;
	}

    } else if (lina == DPMI_ADD + HLT_OFF(MSDOS_PS2_mouse_callback)) {
	unsigned short *rm_ssp;
	if (!ValidAndUsedSelector(MSDOS_CLIENT.PS2mouseCallBack.selector)) {
	    D_printf
		("MSDOS: ERROR: PS2 mouse callback to unused segment\n");
	    return 0;
	}
	D_printf("MSDOS: starting PS2 mouse callback\n");

	_cs = MSDOS_CLIENT.PS2mouseCallBack.selector;
	_eip = MSDOS_CLIENT.PS2mouseCallBack.offset;

	rm_ssp = MK_FP32(RMREG(ss), RMREG(sp) + 4 + 8);

	if (MSDOS_CLIENT.is_32) {
	    unsigned int *ssp = sp;
	    *--ssp = *--rm_ssp;
	    D_printf("data: 0x%x ", *ssp);
	    *--ssp = *--rm_ssp;
	    D_printf("0x%x ", *ssp);
	    *--ssp = *--rm_ssp;
	    D_printf("0x%x ", *ssp);
	    *--ssp = *--rm_ssp;
	    D_printf("0x%x\n", *ssp);
	    *--ssp = dpmi_sel();
	    *--ssp = DPMI_SEL_OFF(MSDOS_return_from_pm);
	    _esp -= 24;
	} else {
	    unsigned short *ssp = sp;
	    *--ssp = *--rm_ssp;
	    D_printf("data: 0x%x ", *ssp);
	    *--ssp = *--rm_ssp;
	    D_printf("0x%x ", *ssp);
	    *--ssp = *--rm_ssp;
	    D_printf("0x%x ", *ssp);
	    *--ssp = *--rm_ssp;
	    D_printf("0x%x\n", *ssp);
	    *--ssp = dpmi_sel();
	    *--ssp = DPMI_SEL_OFF(MSDOS_return_from_pm);
	    _LWORD(esp) -= 12;
	}
    }
    return 1;
}

void msdos_post_rm(struct sigcontext_struct *scp)
{
    pm_to_rm_regs(scp, ~0);
}

int msdos_pre_pm(struct sigcontext_struct *scp,
		 struct RealModeCallStructure *rmreg)
{
    if (_eip == 1 + DPMI_SEL_OFF(MSDOS_XMS_call)) {
	D_printf("MSDOS: XMS call to 0x%x:0x%x\n",
		 MSDOS_CLIENT.XMS_call.segment,
		 MSDOS_CLIENT.XMS_call.offset);
	pm_to_rm_regs(scp, ~0);
	RMREG(cs) = DPMI_SEG;
	RMREG(ip) = DPMI_OFF + HLT_OFF(MSDOS_return_from_rm);
	do_call_to(MSDOS_CLIENT.XMS_call.segment,
		     MSDOS_CLIENT.XMS_call.offset, rmreg);
    } else {
	error("MSDOS: unknown pm call %#x\n", _eip);
	return 0;
    }
    return 1;
}

void msdos_post_pm(struct sigcontext_struct *scp)
{
    rm_to_pm_regs(scp, ~0);
}

void msdos_pm_call(struct sigcontext_struct *scp)
{
    if (_eip == 1 + DPMI_SEL_OFF(MSDOS_API_call)) {
	D_printf("MSDOS: extension API call: 0x%04x\n", _LWORD(eax));
	if (_LWORD(eax) == 0x0100) {
	    _eax = DPMI_ldt_alias();	/* simulate direct ldt access */
	    _eflags &= ~CF;
	} else {
	    _eflags |= CF;
	}
    } else {
	error("MSDOS: unknown pm call %#x\n", _eip);
    }
}

int msdos_fault(struct sigcontext_struct *scp)
{
    struct sigcontext_struct new_sct;
    int reg;
    unsigned int segment;
    unsigned short desc;

    D_printf("MSDOS: msdos_fault, err=%#lx\n", _err);
    if ((_err & 0xffff) == 0)	/*  not a selector error */
	return 0;

    /* now it is a invalid selector error, try to fix it if it is */
    /* caused by an instruction such as mov Sreg,r/m16            */

#define ALL_GDTS 0
#if !ALL_GDTS
    segment = (_err & 0xfff8);
    /* only allow using some special GTDs */
    if ((segment != 0x0040) && (segment != 0xa000) &&
	(segment != 0xb000) && (segment != 0xb800) &&
	(segment != 0xc000) && (segment != 0xe000) &&
	(segment != 0xf000) && (segment != 0xbf8) &&
	(segment != 0xf800) && (segment != 0xff00) && (segment != 0x38))
	return 0;

    copy_context(&new_sct, scp, 0);
    reg = decode_segreg(&new_sct);
    if (reg == -1)
	return 0;
#else
    copy_context(&new_sct, scp, 0);
    reg = decode_modify_segreg_insn(&new_sct, 1, &segment);
    if (reg == -1)
	return 0;

    if (ValidAndUsedSelector(segment)) {
	/*
	 * The selector itself is OK, but the descriptor (type) is not.
	 * We cannot fix this! So just give up immediately and dont
	 * screw up the context.
	 */
	D_printf("MSDOS: msdos_fault: Illegal use of selector %#x\n",
		 segment);
	return 0;
    }
#endif

    D_printf("MSDOS: try mov to a invalid selector 0x%04x\n", segment);

    switch (segment) {
    case 0x38:
	/* dos4gw sets VCPI descriptors 0x28, 0x30, 0x38 */
	/* The 0x38 is the "flat data" segment (0,4G) */
	desc = ConvertSegmentToDescriptor_lim(0, 0xffffffff);
	break;
    default:
	/* any other special cases? */
	desc = (reg != cs_INDEX ? ConvertSegmentToDescriptor(segment) :
		ConvertSegmentToCodeDescriptor(segment));
    }
    if (!desc)
	return 0;

    /* OKay, all the sanity checks passed. Now we go and fix the selector */
    copy_context(scp, &new_sct, 0);
    switch (reg) {
    case es_INDEX:
	_es = desc;
	break;
    case cs_INDEX:
	_cs = desc;
	break;
    case ss_INDEX:
	_ss = desc;
	break;
    case ds_INDEX:
	_ds = desc;
	break;
    case fs_INDEX:
	_fs = desc;
	break;
    case gs_INDEX:
	_gs = desc;
	break;
    }

    /* let's hope we fixed the thing, and return */
    return 1;
}

static void lr_hlt(Bit32u idx, void *arg)
{
    unsigned int offs = REG(edi);
    unsigned int size = REG(ecx);
    unsigned int dos_ptr = SEGOFF2LINEAR(REG(ds), LWORD(edx));
    if (offs + size <= io_buffer_size)
	MEMCPY_2UNIX(io_buffer + offs, dos_ptr, size);
    do_retf();
}

static void lw_hlt(Bit32u idx, void *arg)
{
    unsigned int offs = REG(edi);
    unsigned int size = REG(ecx);
    unsigned int dos_ptr = SEGOFF2LINEAR(REG(ds), LWORD(edx));
    if (offs + size <= io_buffer_size)
	MEMCPY_2DOS(dos_ptr, io_buffer + offs, size);
    do_retf();
}

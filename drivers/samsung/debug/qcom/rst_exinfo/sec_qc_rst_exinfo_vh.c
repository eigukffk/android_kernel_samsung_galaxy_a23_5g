// SPDX-License-Identifier: GPL-2.0
/*
 * COPYRIGHT(C) 2006-2021 Samsung Electronics Co., Ltd. All Right Reserved.
 */

#include <linux/kernel.h>

#include <asm/esr.h>
#include <asm/kasan.h>
#include <asm/pgtable.h>

#include <trace/hooks/bug.h>
#include <trace/hooks/fault.h>

#include "sec_qc_rst_exinfo.h"

static pgd_t *init_mm_pgdp;

#define __rst_exinfo_read_special_reg(x) ({ \
	uint64_t val; \
	asm volatile ("mrs %0, " # x : "=r"(val)); \
	val; \
})

/* FIXME: these functions are imported from 'arch/arm64/mm/fault.c'. */
static inline bool is_ttbr0_addr(unsigned long addr)
{
	/* entry assembly clears tags for TTBR0 addrs */
	return addr < TASK_SIZE;
}

static inline bool is_ttbr1_addr(unsigned long addr)
{
	/* TTBR1 addresses may have a tag if KASAN_SW_TAGS is in use */
	return arch_kasan_reset_tag(addr) >= PAGE_OFFSET;
}

int __qc_rst_exinfo_vh_init(struct builder *bd)
{
	union {
		struct {
			uint64_t baddr:48;
			uint64_t asid:16;
		};
		uint64_t raw;
	} ttbr1_el1;
	phys_addr_t baddr_phys;

	ttbr1_el1.raw = __rst_exinfo_read_special_reg(TTBR1_EL1);
	baddr_phys = ttbr1_el1.baddr & (~(PAGE_SIZE - 1));
	init_mm_pgdp = (pgd_t *)__phys_to_kimg(baddr_phys);

	return 0;
}

static inline void __rst_exinfo_vh_store_pte(_kern_ex_info_t *kern_ex_info,
		unsigned long addr, int idx)
{
	int cpu;

	cpu = get_cpu();

	if (unlikely(idx == 0))
		memset(&kern_ex_info->fault[cpu].pte, 0,
				sizeof(kern_ex_info->fault[cpu].pte));

	kern_ex_info->fault[cpu].pte[idx] = addr;

	put_cpu();
}

static __always_inline void ____rst_exinfo_vh_table_walk(
		_kern_ex_info_t *kern_ex_info, unsigned long addr)
{
	pgd_t *mm_pgd;
	pgd_t *pgdp;
	pgd_t pgd;

	if (is_ttbr0_addr(addr)) {
		/* TTBR0 */
		mm_pgd = current->active_mm->pgd;
		if (mm_pgd == init_mm_pgdp)
			return;
	} else if (is_ttbr1_addr(addr)) {
		/* TTBR1 */
		mm_pgd = init_mm_pgdp;
	} else {
		__rst_exinfo_vh_store_pte(kern_ex_info, addr, 1);
		return;
	}

	__rst_exinfo_vh_store_pte(kern_ex_info, (unsigned long)mm_pgd, 0);
	__rst_exinfo_vh_store_pte(kern_ex_info, addr, 1);

	pgdp = pgd_offset_raw(mm_pgd, addr);
	pgd = READ_ONCE(*pgdp);
	__rst_exinfo_vh_store_pte(kern_ex_info, (unsigned long)pgd_val(pgd), 2);

	do {
		pud_t *pudp, pud;
		pmd_t *pmdp, pmd;
		pte_t *ptep, pte;

		if (pgd_none(pgd) || pgd_bad(pgd))
			break;

		pudp = pud_offset(pgdp, addr);
		pud = READ_ONCE(*pudp);
		__rst_exinfo_vh_store_pte(kern_ex_info,
				(unsigned long)pud_val(pud), 3);
		if (pud_none(pud) || pud_bad(pud))
			break;

		pmdp = pmd_offset(pudp, addr);
		pmd = READ_ONCE(*pmdp);
		__rst_exinfo_vh_store_pte(kern_ex_info,
				(unsigned long)pmd_val(pmd), 4);
		if (pmd_none(pmd) || pmd_bad(pmd))
			break;

		ptep = pte_offset_map(pmdp, addr);
		pte = READ_ONCE(*ptep);
		pte_unmap(ptep);
		__rst_exinfo_vh_store_pte(kern_ex_info,
				(unsigned long)pte_val(pte), 5);
	} while (0);
}

static inline void __rst_exinfo_vh_table_walk(unsigned long addr)
{
	rst_exinfo_t *rst_exinfo;
	_kern_ex_info_t *kern_ex_info;

	if (!__qc_rst_exinfo_is_probed())
		return;

	rst_exinfo = qc_rst_exinfo->rst_exinfo;
	kern_ex_info = &rst_exinfo->kern_ex_info.info;
	if (kern_ex_info->cpu != -1)
		return;

	____rst_exinfo_vh_table_walk(kern_ex_info, addr);
}

static __always_inline void ____rst_exinfo_save_fault_info(
		_kern_ex_info_t *kern_ex_info,  unsigned int esr,
		const char *str, unsigned long var1, unsigned long var2)
{
	int cpu;

	cpu = get_cpu();

	kern_ex_info->fault[cpu].esr = esr;
	snprintf(kern_ex_info->fault[cpu].str,
			sizeof(kern_ex_info->fault[cpu].str),
			"%s", str);
	kern_ex_info->fault[cpu].var1 = var1;
	kern_ex_info->fault[cpu].var2 = var2;

	put_cpu();
}

static inline void __rst_exinfo_save_fault_info(unsigned int esr,
		const char *str, unsigned long var1, unsigned long var2)
{
	rst_exinfo_t *rst_exinfo;
	_kern_ex_info_t *kern_ex_info;

	if (!__qc_rst_exinfo_is_probed())
		return;

	rst_exinfo = qc_rst_exinfo->rst_exinfo;
	kern_ex_info = &rst_exinfo->kern_ex_info.info;
	if (kern_ex_info->cpu != -1)
		return;

	____rst_exinfo_save_fault_info(kern_ex_info, esr, str, var1, var2);
}

static void sec_qc_rst_exinfo_rvh_die_kernel_falut(void *unused,
		struct pt_regs *regs, unsigned int esr,
		unsigned long addr, const char *msg)
{
	__qc_rst_exinfo_store_extc_idx(false);
}

int __qc_rst_exinfo_register_rvh_die_kernel_fault(struct builder *bd)
{
	return register_trace_android_rvh_die_kernel_fault(
			sec_qc_rst_exinfo_rvh_die_kernel_falut, NULL);
}

static void sec_qc_rst_exinfo_rvh_do_mem_abort(void *unused,
		struct pt_regs *regs, unsigned int esr,
		unsigned long addr, const char *msg)
{
	if (!user_mode(regs)) {
		__rst_exinfo_save_fault_info(esr, msg, addr, 0UL);
		__rst_exinfo_vh_table_walk(addr);
	}
}

int __qc_rst_exinfo_register_rvh_do_mem_abort(struct builder *bd)
{
	return register_trace_android_rvh_do_mem_abort(
			sec_qc_rst_exinfo_rvh_do_mem_abort, NULL);
}

/* FIXME: this is copied data from 'arch/arm64/kernel/traps.c' */
static const char *__rst_exinfo_esr_class_str[] = {
	[0 ... ESR_ELx_EC_MAX]		= "UNRECOGNIZED EC",
	[ESR_ELx_EC_UNKNOWN]		= "Unknown/Uncategorized",
	[ESR_ELx_EC_WFx]		= "WFI/WFE",
	[ESR_ELx_EC_CP15_32]		= "CP15 MCR/MRC",
	[ESR_ELx_EC_CP15_64]		= "CP15 MCRR/MRRC",
	[ESR_ELx_EC_CP14_MR]		= "CP14 MCR/MRC",
	[ESR_ELx_EC_CP14_LS]		= "CP14 LDC/STC",
	[ESR_ELx_EC_FP_ASIMD]		= "ASIMD",
	[ESR_ELx_EC_CP10_ID]		= "CP10 MRC/VMRS",
	[ESR_ELx_EC_PAC]		= "PAC",
	[ESR_ELx_EC_CP14_64]		= "CP14 MCRR/MRRC",
	[ESR_ELx_EC_ILL]		= "PSTATE.IL",
	[ESR_ELx_EC_SVC32]		= "SVC (AArch32)",
	[ESR_ELx_EC_HVC32]		= "HVC (AArch32)",
	[ESR_ELx_EC_SMC32]		= "SMC (AArch32)",
	[ESR_ELx_EC_SVC64]		= "SVC (AArch64)",
	[ESR_ELx_EC_HVC64]		= "HVC (AArch64)",
	[ESR_ELx_EC_SMC64]		= "SMC (AArch64)",
	[ESR_ELx_EC_SYS64]		= "MSR/MRS (AArch64)",
	[ESR_ELx_EC_SVE]		= "SVE",
	[ESR_ELx_EC_ERET]		= "ERET/ERETAA/ERETAB",
	[ESR_ELx_EC_IMP_DEF]		= "EL3 IMP DEF",
	[ESR_ELx_EC_IABT_LOW]		= "IABT (lower EL)",
	[ESR_ELx_EC_IABT_CUR]		= "IABT (current EL)",
	[ESR_ELx_EC_PC_ALIGN]		= "PC Alignment",
	[ESR_ELx_EC_DABT_LOW]		= "DABT (lower EL)",
	[ESR_ELx_EC_DABT_CUR]		= "DABT (current EL)",
	[ESR_ELx_EC_SP_ALIGN]		= "SP Alignment",
	[ESR_ELx_EC_FP_EXC32]		= "FP (AArch32)",
	[ESR_ELx_EC_FP_EXC64]		= "FP (AArch64)",
	[ESR_ELx_EC_SERROR]		= "SError",
	[ESR_ELx_EC_BREAKPT_LOW]	= "Breakpoint (lower EL)",
	[ESR_ELx_EC_BREAKPT_CUR]	= "Breakpoint (current EL)",
	[ESR_ELx_EC_SOFTSTP_LOW]	= "Software Step (lower EL)",
	[ESR_ELx_EC_SOFTSTP_CUR]	= "Software Step (current EL)",
	[ESR_ELx_EC_WATCHPT_LOW]	= "Watchpoint (lower EL)",
	[ESR_ELx_EC_WATCHPT_CUR]	= "Watchpoint (current EL)",
	[ESR_ELx_EC_BKPT32]		= "BKPT (AArch32)",
	[ESR_ELx_EC_VECTOR32]		= "Vector catch (AArch32)",
	[ESR_ELx_EC_BRK64]		= "BRK (AArch64)",
};

static const char *__rst_exinfo_esr_get_class_string(u32 esr)
{
	return __rst_exinfo_esr_class_str[ESR_ELx_EC(esr)];
}

static void sec_qc_rst_exinfo_rvh_do_sp_pc_abort(void *unused,
		struct pt_regs *regs, unsigned int esr,
		unsigned long addr, bool user)
{
	__rst_exinfo_save_fault_info(esr, __rst_exinfo_esr_get_class_string(esr),
			regs->pc, regs->sp);
}

int __qc_rst_exinfo_register_rvh_do_sp_pc_abort(struct builder *bd)
{
	return register_trace_android_rvh_do_sp_pc_abort(
			sec_qc_rst_exinfo_rvh_do_sp_pc_abort, NULL);
}

static void __rst_exinfo_debug_store_bug_string(const char *fmt, ...)
{
	rst_exinfo_t *rst_exinfo;
	_kern_ex_info_t *kern_ex_info;
	va_list args;

	if (!__qc_rst_exinfo_is_probed())
		return;

	rst_exinfo = qc_rst_exinfo->rst_exinfo;
	kern_ex_info = &rst_exinfo->kern_ex_info.info;
	va_start(args, fmt);
	vsnprintf(kern_ex_info->bug_buf,
		sizeof(kern_ex_info->bug_buf), fmt, args);
	va_end(args);
}

#define MAX_BUG_STRING_SIZE		56

static void sec_qc_rst_exinfo_rvh_report_bug(void *unused,
		const char *file, unsigned line, unsigned long bugaddr)
{
	const char *token;
	size_t length;

	if (!file)
		return;

	token = strstr(file, "kernel");
	if (!token)
		token = file;

	length = strlen(token);
	if (length > MAX_BUG_STRING_SIZE)
		__rst_exinfo_debug_store_bug_string("%s %u",
				&token[length - MAX_BUG_STRING_SIZE], line);
	else
		__rst_exinfo_debug_store_bug_string("%s %u", token, line);
}

int __qc_rst_exinfo_register_rvh_report_bug(struct builder *bd)
{
	return register_trace_android_rvh_report_bug(
			sec_qc_rst_exinfo_rvh_report_bug, NULL);
}

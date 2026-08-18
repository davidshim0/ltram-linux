/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * LtRAM — Micron MT35XU02GCBA octal-DDR NOR flash presented as coherent memory
 * through the Enzian FPGA's ECI window.
 *
 * THE ONE HARDWARE FACT EVERYTHING FOLLOWS FROM:
 *
 *   The CPU can LOAD from this window coherently. It can never STORE to it.
 *   A store is not faulted or reported -- the instruction retires normally and
 *   the data is silently discarded. A later load returns the previous flash
 *   contents, which look entirely plausible.
 *
 * So every mechanism here exists to manufacture a signal the hardware does not
 * give us. Data reaches the array only through the FPGA's DMA descriptor ring,
 * driven by the NOR driver.
 */
#ifndef _LINUX_LTRAM_H
#define _LINUX_LTRAM_H

#include <linux/types.h>
#include <linux/init.h>

/* The coherent NOR read window. RD_BASE carries the node bit (40) and the
 * marker bit (38) that separates NOR traffic from DMA traffic on the shared
 * coherent VCs. 65536 sectors x 4 KiB = 256 MiB. */
#define LTRAM_PHYS_BASE		0x14000000000ULL
#define LTRAM_PHYS_SIZE		(256ULL << 20)
#define LTRAM_NUMA_NODE		1

#ifdef CONFIG_LTRAM

extern unsigned long ltram_start_pfn, ltram_end_pfn;

/*
 * The LtRAM test. Two compares against constants, no memory reference, no
 * struct page dereference -- it runs in the fault path and once per page in
 * every scanner sweep, so its cost is worth caring about. The window is
 * physically contiguous by construction, which is what makes the cheapest
 * predicate also the available one.
 */
static inline bool pfn_is_ltram(unsigned long pfn)
{
	return pfn >= ltram_start_pfn && pfn < ltram_end_pfn;
}

void __init ltram_declare_node(void);
void ltram_note_stray_alloc(unsigned long pfn, const char *where);

#else  /* !CONFIG_LTRAM */

static inline bool pfn_is_ltram(unsigned long pfn) { return false; }
static inline void ltram_declare_node(void) { }
static inline void ltram_note_stray_alloc(unsigned long pfn, const char *w) { }

#endif /* CONFIG_LTRAM */
#endif /* _LINUX_LTRAM_H */

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
#include <linux/errno.h>

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

/*
 * The write backend.
 *
 * mm/ owns the zone and the policy; it must know nothing about the FPGA. The
 * NOR driver fills this in and registers it, so the only coupling is one
 * function pointer.
 */
struct folio;

struct ltram_flash_ops {
	struct module *owner;
	/*
	 * Erase and program one PAGE_SIZE region.  @dst_pfn is inside
	 * ZONE_LTRAM, @src is a kernel-mapped source page.
	 *
	 * MAY SLEEP -- an erase is ~16.4 ms and the DMA ~1.3 ms.  Must not
	 * return until the data is durably committed: the caller publishes the
	 * migration immediately afterwards, and there is no second chance to
	 * notice a failure.
	 *
	 * The DMA programs the array behind the CPU's back, so a cached copy of
	 * the destination line can be stale.  Any read-back verify inside the
	 * implementation must evict first, with a physical address -- on this
	 * machine `dc civac` is a no-op because the LLC is the point of
	 * coherence, so a verify without eviction re-reads the very line it is
	 * meant to check and always passes.
	 *
	 * Returns 0 on success, negative errno on failure.
	 */
	int (*write_page)(unsigned long dst_pfn, const void *src);
};

int  ltram_register_flash_ops(const struct ltram_flash_ops *ops);
void ltram_unregister_flash_ops(const struct ltram_flash_ops *ops);
bool ltram_have_flash_ops(void);
int  ltram_write_page(unsigned long dst_pfn, const void *src);

bool folio_is_ltram(const struct folio *folio);
int  ltram_copy_to_flash(struct folio *dst, struct folio *src);

void __init ltram_declare_node(void);
void ltram_note_stray_alloc(unsigned long pfn, const char *where);

#else  /* !CONFIG_LTRAM */

static inline bool pfn_is_ltram(unsigned long pfn) { return false; }
static inline void ltram_declare_node(void) { }
static inline void ltram_note_stray_alloc(unsigned long pfn, const char *w) { }
static inline bool ltram_have_flash_ops(void) { return false; }
static inline int ltram_write_page(unsigned long p, const void *s) { return -ENODEV; }
static inline bool folio_is_ltram(const struct folio *f) { return false; }
static inline int ltram_copy_to_flash(struct folio *d, struct folio *s) { return -ENODEV; }

#endif /* CONFIG_LTRAM */
#endif /* _LINUX_LTRAM_H */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cosmo boot markers, later stages (DEBUG, not for submission).
 *
 * See the cosmo_mark macro in head.S for what this is and why it exists. Stages 1 and 2 run with the MMU
 * off and write physical addresses directly. From __enable_mmu onwards that is no longer possible, so
 * these stages go through the linear map, which is why the first of them cannot run before paging_init.
 *
 * The pstore reservation deliberately has no no-map property -- it has to be mapped for pstore to read it
 * back -- so it is part of the linear map and phys_to_virt applies.
 */
#include <linux/cache.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/cacheflush.h>

/* Must match the reservation in mt6771-planet-cosmo.dts and the macro in head.S. */
#define COSMO_MARK_BASE		0x54410000UL
#define COSMO_MARK_END		0x544f0000UL
#define COSMO_MARK_STRIDE	0x1000UL
/* fs/pstore/ram_core.c in the vendor kernel: #define PERSISTENT_RAM_SIG (0x43474244) */
#define PERSISTENT_RAM_SIG	0x43474244
#define COSMO_MARK_LEN		12

void cosmo_mark(const char *tag)
{
	phys_addr_t phys;

	for (phys = COSMO_MARK_BASE; phys < COSMO_MARK_END; phys += COSMO_MARK_STRIDE) {
		u32 *buf = (u32 *)phys_to_virt(phys);

		memcpy(&buf[3], tag, COSMO_MARK_LEN);
		buf[1] = COSMO_MARK_LEN;		/* start */
		buf[2] = COSMO_MARK_LEN;		/* size  */
		/* Signature last, so a torn write is never mistaken for a valid record. */
		buf[0] = PERSISTENT_RAM_SIG;
		/*
		 * The record is read back by the next boot, which never sees our caches, so it has to reach
		 * DRAM rather than sit in a dirty line.
		 */
		dcache_clean_inval_poc((unsigned long)buf, (unsigned long)buf + 24);
	}
}

/* Stage 4: initcalls run, so the kernel got through the whole of start_kernel's setup. */
static int __init cosmo_mark_initcall(void)
{
	cosmo_mark("COSMO-MARK-4");
	return 0;
}
early_initcall(cosmo_mark_initcall);

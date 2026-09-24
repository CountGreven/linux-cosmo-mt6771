// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cosmo boot markers, later stages (DEBUG, not for submission).
 *
 * See the cosmo_mark macro in head.S for what this is and why it exists. Stages 1 and 2 run with the MMU
 * off and write physical addresses directly. From __enable_mmu onwards that is no longer possible, so
 * these stages go through the linear map, which is why the first of them cannot run before paging_init.
 *
 * All of these run BEFORE ramoops probes, and that is now deliberate. ramoops_probe saves the zone's old
 * contents and then zaps it, so a marker written before probe is wiped by probe and cannot interfere with
 * the console log that follows. Markers placed after it do interfere: a COSMO-PROBE2 at the end of probe,
 * and a COSMO-CONSOLE inside pstore_register_console, each erased a log that was very likely working.
 * Both are gone. The reading is now unambiguous:
 *
 *   kernel log text   pstore worked; read it, and take these markers out too.
 *   a stage tag       ramoops never probed, so nothing zapped the zone.
 *   nothing at all    probe ran and zapped, and no console output followed it.
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

/*
 * Stages 4 to 7 walk the initcall levels. They deliberately stop before device_initcall, which is where
 * ramoops itself probes: from that point pstore owns this buffer, and a marker written afterwards would
 * overwrite the real console log we are trying to reach. So if a run leaves stage 7 behind, the kernel
 * died between fs_initcall and pstore coming up; if it leaves actual kernel log text instead, pstore
 * worked and these markers have done their job and should come out.
 */
#define COSMO_MARK_STAGE(level, n)					\
static int __init cosmo_mark_##level(void)				\
{									\
	cosmo_mark("COSMO-MARK-" #n);					\
	return 0;							\
}									\
level##_initcall(cosmo_mark_##level)

COSMO_MARK_STAGE(early, 4);	/* start_kernel's setup completed */

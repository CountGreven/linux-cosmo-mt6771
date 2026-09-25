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
#include <linux/minmax.h>
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
/* First page of the pmsg zone: base + size - pmsg_size, read back as pmsg-ramoops-0. */
#define COSMO_TAG_PAGE		0x544e0000UL
/* Header of ramoops' console zone: base + (size - console - pmsg). Read back to see whether its writes land. */
#define COSMO_CONSOLE_ZONE	0x544a0000UL

/*
 * Set by ramoops_probe before it initialises the first zone. Until then a marker is sprayed across the
 * whole reservation (every page, so the vendor's reader finds it whatever zone layout it assumes) and
 * probe wipes it. From then on pstore owns the zones and the log we want lives in the console zone, so
 * every marker is instead rendered onto ONE page: the first page of the pmsg zone, which the vendor
 * kernel reads back as pmsg-ramoops-0. Nothing after the claim touches the console zone.
 *
 * The page carries one status line: the last stage tag, how many times the console write was entered
 * and whether the last one returned (E = entered, K = returned), and the initcall currently running.
 * A spray after the claim erased four logs; the kill switch that replaced it hid the second write.
 */
bool cosmo_mark_claimed;
static char cosmo_last_tag[COSMO_MARK_LEN + 1] = "-";
static char cosmo_ic[40] = "-";
static unsigned int cosmo_pw_n;
static char cosmo_pw_state = '-';

static void cosmo_write_page(phys_addr_t phys, const char *text, size_t len)
{
	u32 *buf = (u32 *)phys_to_virt(phys);

	memcpy(&buf[3], text, len);
	buf[1] = len;				/* start */
	buf[2] = len;				/* size  */
	/* Signature last, so a torn write is never mistaken for a valid record. */
	buf[0] = PERSISTENT_RAM_SIG;
	/*
	 * The record is read back by the next boot, which never sees our caches, so it has to reach DRAM
	 * rather than sit in a dirty line.
	 */
	dcache_clean_inval_poc((unsigned long)buf, (unsigned long)buf + 12 + len);
}

static void cosmo_spray(const char *text, size_t len)
{
	phys_addr_t phys;

	for (phys = COSMO_MARK_BASE; phys < COSMO_MARK_END; phys += COSMO_MARK_STRIDE)
		cosmo_write_page(phys, text, len);
}

static void cosmo_render(void)
{
	char line[64];
	u32 *cz = (u32 *)phys_to_virt(COSMO_CONSOLE_ZONE);
	int n;

	/*
	 * ramoops writes the console zone through a write-combined alias. Invalidate our cacheable alias
	 * first, so this read comes from DRAM and shows what the next boot will see: start/size of the
	 * zone header, which should grow with every console write.
	 */
	dcache_inval_poc((unsigned long)cz, (unsigned long)cz + 16);
	n = snprintf(line, sizeof(line), "%s PW%u%c CZ%u/%u %s", cosmo_last_tag, cosmo_pw_n,
		     cosmo_pw_state, cz[1], cz[2], cosmo_ic);

	cosmo_write_page(COSMO_TAG_PAGE, line, n > 0 ? min_t(size_t, n, sizeof(line)) : 0);
}

void cosmo_mark_len(const char *buf_in, size_t len)
{
	if (len > 64)
		len = 64;
	if (!cosmo_mark_claimed) {
		cosmo_spray(buf_in, len);
		return;
	}
	strscpy(cosmo_ic, buf_in, sizeof(cosmo_ic));
	cosmo_render();
}

void cosmo_mark(const char *tag)
{
	memcpy(cosmo_last_tag, tag, COSMO_MARK_LEN);
	cosmo_last_tag[COSMO_MARK_LEN] = '\0';
	if (!cosmo_mark_claimed) {
		cosmo_spray(tag, COSMO_MARK_LEN);
		return;
	}
	cosmo_render();
}

/* Called around psinfo->write in pstore_console_write: false on entry, true when the write returned. */
void cosmo_note_pw(bool returned)
{
	if (!returned)
		cosmo_pw_n++;
	cosmo_pw_state = returned ? 'K' : 'E';
	cosmo_render();
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
/*
 * Stages 5 and 6 bisect the gap between early_initcall and ramoops.
 *
 * With ramoops.mem_address on the cmdline, ramoops_init registers its own platform device at
 * postcore_initcall. The levels are pure(1), core(2), postcore(3), so both of these still run before it
 * and are still wiped by the zap when probe happens -- they cannot overwrite a log.
 */
COSMO_MARK_STAGE(pure, 5);
COSMO_MARK_STAGE(core, 6);

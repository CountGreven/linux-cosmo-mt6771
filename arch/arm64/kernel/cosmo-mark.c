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
#include <linux/io.h>
#include <asm/early_ioremap.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>

/* Must match the reservation in mt6771-planet-cosmo.dts and the macro in head.S. */
#define COSMO_MARK_BASE		0x54410000UL
#define COSMO_MARK_END		0x544f0000UL
/* fs/pstore/ram_core.c in the vendor kernel: #define PERSISTENT_RAM_SIG (0x43474244) */
#define PERSISTENT_RAM_SIG	0x43474244
#define COSMO_MARK_LEN		12
/* The vendor's first console zone (base + 0x50000), read back as console-ramoops. */
#define COSMO_TAG_PAGE		0x54460000UL
/* Header of ramoops' console zone: base + (size - console - pmsg). Read back to see whether its writes land. */
#define COSMO_CONSOLE_ZONE	0x544e0000UL
#define COSMO_CONSOLE_SIZE	0x10000UL
#define COSMO_CONSOLE_PAGES	(COSMO_CONSOLE_SIZE / PAGE_SIZE)

/*
 * Every C-side marker is rendered onto ONE page: the first page of the pmsg zone, which the vendor
 * kernel reads back as pmsg-ramoops-0. Nothing written from C ever touches the console zone.
 *
 * That is the lesson of the spray: markers written through the cacheable linear map and cleaned to
 * PoC still came back after a reset on top of what ramoops had written through its write-combined
 * alias -- the kernel's own readout (CZ below) saw 25 KB of log in the zone, the next boot saw the
 * spray header. Only the MMU-off stages in head.S still write the whole reservation, and ramoops
 * zaps those at probe.
 *
 * The page carries one status line: the last stage tag, how many times the console write was entered
 * and whether the last one returned (E = entered, K = returned), the console zone header as read back
 * from DRAM, and the initcall currently running. Before ramoops probes, the pmsg zone is scratch; the
 * probe zaps it, and everything after the claim is rendered again, so a tag that survives is either
 * from before probe (the kernel died there) or from after it.
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


/*
 * The console zone is only ever looked at through a write-combined vmap, never through the cacheable
 * linear map: an earlier readout through phys_to_virt left a valid cache line for the zone header,
 * and that line is the prime suspect for the header coming back stale after every reset (a
 * non-cacheable store that hits a valid line may be served by the cache on these cores).
 */
static void __iomem *cosmo_nc;
/* E4: what DRAM held in the console zone when this boot claimed it -- the previous run's leftovers. */
static char cosmo_prev[160];
static char cosmo_rgu[48] = "RGU -";

static void cosmo_snapshot_prev(void)
{
	u8 d12[16], m800[16];

	memcpy_fromio(d12, cosmo_nc + 12, sizeof(d12));
	memcpy_fromio(m800, cosmo_nc + 0x800, sizeof(m800));
	snprintf(cosmo_prev, sizeof(cosmo_prev), "PREV %u/%u d12=%16phN m800=%16phN",
		 readl(cosmo_nc + 4), readl(cosmo_nc + 8), d12, m800);
}

static void cosmo_render(void)
{
	char line[64];
	u32 *hdr;
	u32 cz_start = 0, cz_size = 0;
	int n;

	if (cosmo_mark_claimed && !cosmo_nc) {
		struct page *pages[COSMO_CONSOLE_PAGES];
		int i;

		for (i = 0; i < COSMO_CONSOLE_PAGES; i++)
			pages[i] = phys_to_page(COSMO_CONSOLE_ZONE + i * PAGE_SIZE);
		cosmo_nc = vmap(pages, COSMO_CONSOLE_PAGES, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
		if (cosmo_nc)
			cosmo_snapshot_prev();
	}
	if (cosmo_nc) {
		cz_start = readl(cosmo_nc + 4);
		cz_size = readl(cosmo_nc + 8);
	}
	n = snprintf(line, sizeof(line), "%s PW%u%c CZ%u/%u %s", cosmo_last_tag, cosmo_pw_n,
		     cosmo_pw_state, cz_start, cz_size, cosmo_ic);
	n = n > 0 ? min_t(size_t, n, sizeof(line)) : 0;
	cosmo_write_page(COSMO_TAG_PAGE, line, n);
	if (!cosmo_mark_claimed)
		return;
	/*
	 * E4/RGU: the snapshot at +0x100 and the RGU registers at +0x1c0 of the tag page. The header
	 * declares up to the end of those and never past this page: the tag page sits inside a 4 KiB
	 * ramoops dump zone, and a header claiming more made ramoops_pstore_read() run off the end of
	 * its mapping when systemd mounted pstore (the first oops of this port, on the first boot to init).
	 */
	hdr = (u32 *)phys_to_virt(COSMO_TAG_PAGE);
	memcpy((u8 *)hdr + 0x100, cosmo_prev, sizeof(cosmo_prev));
	memcpy((u8 *)hdr + 0x1c0, cosmo_rgu, sizeof(cosmo_rgu));
	hdr[1] = hdr[2] = 0x1c0 + sizeof(cosmo_rgu) - 12;
	dcache_clean_inval_poc((unsigned long)hdr, (unsigned long)hdr + 0x1c0 + sizeof(cosmo_rgu));
}

/*
 * Called once from setup_arch, on the boot CPU, before any secondary is up and before ramoops runs.
 *
 * The MMU-off stages in head.S store into the reservation with the D-cache disabled. On this SoC that
 * does not bypass the cluster L2: LK leaves valid lines behind, the stores hit them and dirty exactly
 * the bytes they wrote, and nothing coherent ever cleans them -- ramoops' write-combined records
 * landed in DRAM (E4 snapshot: the boot log was there), but at the reset-time cache flush those dirty
 * lines were written back and restored precisely the 24 head.S bytes on every page. Clean and
 * invalidate the whole reservation through the linear map now, while we are still on the CPU that
 * holds those lines, so nothing stale survives into the pstore era.
 */
/*
 * MediaTek RGU (watchdog/reset unit) at 0x10007000. The vendor kernel sets MCU_CACHE_PRESERVE in
 * DEBUG_CTL so the cluster caches survive a watchdog reset for its last-PC dumps, and nothing on the
 * way to us clears it. With it set, what we write into ramoops can sit dirty in a preserved cache
 * line through the reset: the next kernel reads stale DRAM, and our bytes only land when something
 * later flushes all caches (the E4 snapshot saw the previous run's complete log one boot late).
 * Record the registers on the tag page, then clear the bit with its key.
 */
#define COSMO_RGU_BASE		0x10007000UL
#define RGU_MODE		0x00
#define RGU_STATUS		0x0c
#define RGU_DEBUG_CTL		0x40
#define RGU_LATCH_CTL		0x44
#define RGU_DEBUG_CTL_KEY	0x59000000
#define RGU_MCU_CACHE_PRESERVE	0x00000008
static void __init cosmo_rgu_probe(void)
{
	void __iomem *rgu = early_ioremap(COSMO_RGU_BASE, 0x100);
	u32 mode, status, dbg, latch;

	if (!rgu)
		return;
	mode = readl(rgu + RGU_MODE);
	status = readl(rgu + RGU_STATUS);
	dbg = readl(rgu + RGU_DEBUG_CTL);
	latch = readl(rgu + RGU_LATCH_CTL);
	snprintf(cosmo_rgu, sizeof(cosmo_rgu), "RGU m=%x s=%x d=%x l=%x", mode, status, dbg, latch);
	if (dbg & RGU_MCU_CACHE_PRESERVE)
		writel((dbg & ~RGU_MCU_CACHE_PRESERVE) | RGU_DEBUG_CTL_KEY, rgu + RGU_DEBUG_CTL);
	early_iounmap(rgu, 0x100);
}

void __init cosmo_mark_scrub(void)
{
	cosmo_rgu_probe();
	unsigned long start = (unsigned long)phys_to_virt(COSMO_MARK_BASE);

	dcache_clean_inval_poc(start, start + (COSMO_MARK_END - COSMO_MARK_BASE));
}

void cosmo_mark_len(const char *buf_in, size_t len)
{
	if (len > 64)
		len = 64;
	strscpy(cosmo_ic, buf_in, sizeof(cosmo_ic));
	cosmo_render();
}

void cosmo_mark(const char *tag)
{
	memcpy(cosmo_last_tag, tag, COSMO_MARK_LEN);
	cosmo_last_tag[COSMO_MARK_LEN] = '\0';
	cosmo_render();
}

/*
 * Mirror [off, off + len) of the console zone through the cacheable linear map and clean it to PoC.
 *
 * Every run so far: what this kernel writes into the zone through ramoops' write-combined alias is
 * complete in DRAM by the time the *next* run of this kernel looks (E4 snapshot), yet the vendor
 * kernel booting in between reads a stale header and exposes nothing. What the vendor has read back
 * correctly every single time is the tag page, written cacheably and cleaned to PoC. So give the
 * console records the same treatment: read the bytes back through the write-combined alias, store
 * them through the linear map, clean and invalidate. Same PA, same bytes; only the path differs.
 */
static void cosmo_mirror(size_t off, size_t len)
{
	u8 *lin = (u8 *)phys_to_virt(COSMO_CONSOLE_ZONE) + off;

	memcpy_fromio(lin, cosmo_nc + off, len);
	dcache_clean_inval_poc((unsigned long)lin, (unsigned long)lin + len);
}

/* Called around psinfo->write in pstore_console_write: false on entry, true when the write returned. */
void cosmo_note_pw(bool returned, size_t len)
{
	if (!returned)
		cosmo_pw_n++;
	cosmo_pw_state = returned ? 'K' : 'E';
	if (returned && cosmo_nc) {
		/* persistent_ram_write: start is the write pointer after this record, wrapping at data size. */
		size_t data = COSMO_CONSOLE_SIZE - 12;
		size_t start = readl(cosmo_nc + 4) % data;
		size_t from = (start + data - (len % data)) % data;

		if (len > data)
			len = data;
		if (from + len <= data) {
			cosmo_mirror(12 + from, len);
		} else {
			cosmo_mirror(12 + from, data - from);
			cosmo_mirror(12, len - (data - from));
		}
		cosmo_mirror(0, 12);				/* sig, start, size */
	}
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

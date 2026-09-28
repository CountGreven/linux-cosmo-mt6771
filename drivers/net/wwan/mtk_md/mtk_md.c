// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6771 integrated modem (MD1, generation 6293): platform driver skeleton.
 *
 * The bootloader loads and authenticates the modem firmware and describes what it loaded in
 * /chosen. This driver takes over where the vendor's eccci does in md_cd_start()
 * (eccci/modem_sys1.c in the 4.4 BSP): clocks, CCIF and CLDMA reset, modem power-on and release,
 * CLDMA start, and then the first exchange over CCIF SRAM -- the modem's feature query (HS1) and
 * the AP's runtime data. Every step is logged, numbered as in the design note (14-modem.org).
 *
 * The MD1 power switch is driven from here through the SPM and infracfg syscons: mainline's
 * MT8183 power domains do not include it. The handshake stops after the runtime data: HS2 and
 * everything after it need the CCIF ring queues.
 */

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/skbuff.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>

#include "mtk_md_proto.h"
#include "mtk_md_regs.h"

/* BOOT_TIMEOUT in eccci/fsm/ccci_fsm_internal.h covers HS1 and HS2 together */
#define MTK_MD_HS_TIMEOUT_MS	30000
/*
 * The modem's share memory starts with LK's tag list and has to be zeroed before the modem
 * starts. A copy kept in the unassigned tail (the 6293 layout ends at 986 KiB of 1 MiB) lets the
 * driver be bound again without a reboot.
 */
#define MTK_MD_LK_STASH_OFFSET	(SZ_1M - SZ_8K)
#define MTK_MD_LK_STASH_MAGIC	0x4b4c444d	/* "MDLK" */

#define MTK_MD_SMEM_CCISM_OFFSET	(160 * SZ_1K)
#define MTK_MD_SMEM_CCISM_SIZE	(705 * SZ_1K)
#define MTK_MD_SMEM_CCISM_EXP_OFFSET	(865 * SZ_1K)
#define MTK_MD_SMEM_CCISM_EXP_SIZE	(121 * SZ_1K)
#define MTK_MD_TX_SEQ_CHANNELS	256
#define MTK_MD_RX_LOG_LIMIT	200	/* messages described in the log while ports are missing */

/*
 * The cacheable share memory as the vendor lays it out on 6293 (md1_6293_cacheable[] and
 * ccb_configs[]): CCB data first, then the DHL raw buffer at 2 MiB.
 */
#define MTK_MD_CCB_DATA_SIZE	0x17c000
#define MTK_MD_DHL_OFFSET	SZ_2M
#define MTK_MD_DHL_SIZE		(20 * SZ_1M)
#define MTK_MD_BANK4_BASE	0x40000000	/* the modem's view of the AP's memory */
#define MTK_MD_CACHE_OFFSET_DEF	0x8000000	/* when LK gives no md1_smem_cahce_offset */
/* booting_start_id: mdwait_time 15 as the vendor's ccci_mdinit sets it, logging idle */
#define MTK_MD_BOOTING_START_ID	0x000f0000

/* RPC answers (port_rpc.h), and what the modem asks about its SIM hot-plug interrupts */
#define MTK_MD_RPC_TXQ			1
#define RPC_GET_TDD_EINT_NUM		0x4001
#define RPC_GET_GPIO_NUM		0x4002
#define RPC_GET_ADC_NUM			0x4003
#define RPC_GET_EINT_ATTR		0x4005
#define RPC_GET_GPIO_VAL		0x4006
#define RPC_GET_ADC_VAL			0x4007
#define RPC_LHIF_MAPPING		0x400d
#define RPC_DTSI_QUERY			0x400e
#define RPC_TRNG			0x4012
#define RPC_ERR_NO_OP			(-1)
#define RPC_ERR_PARAM			(-2)
#define RPC_ERR_FUNC_FAIL		(-5)
#define RPC_ERR_SIM_QUERY_TYPE		(-12)
#define RPC_ERR_SIM_QUERY_STRING	(-11)
#define MTK_MD_SIM_MAX			4
#define MTK_MD_SIM_ATTRS		7	/* sim_hot_plug_eint_queryType */
#define MTK_MD_DTSI_OUT_LEN		68	/* u32 value, char string[64] */

/*
 * The modem's file service runs in user space, as the vendor's ccci_fsd does: /dev/mtk_md_fs
 * carries whole CCCI messages, one per read or write, on the file channels of queue 4.
 */
#define MTK_MD_FS_Q		4
#define MTK_MD_FS_MAX_LEN	4096	/* the modem sends at most 3456 bytes of data per message */
#define MTK_MD_FS_RX_BACKLOG	32

#define MTK_MD_POLL_US		1000
#define MTK_MD_POLL_TIMEOUT_US	1000000

/* The CCCI MTU (3456) plus the CCCI header's allowance, less the header: AP_CCMNI_MTU */
#define MTK_MD_CCMNI_MTU	(CLDMA_MTU_SIZE - sizeof(struct mtk_md_ccci_hdr))

enum { MTK_MD_RST_CLDMA_AO, MTK_MD_RST_CLDMA_PD, MTK_MD_RST_CCIF, MTK_MD_RST_NUM };

/*
 * "dvfsrc": the modem posts its voltage and memory frequency requests to the DVFSRC as soon as it
 * has its runtime data. With that block's bus clock gated the write never completes and holds
 * the interconnect: the whole SoC stops about 2.6 ms after the runtime data.
 */
static const char * const mtk_md_clk_names[] = {
	"cldma", "ccif-ap", "ccif-md", "ccif1-ap", "ccif1-md", "ccif2-ap", "ccif2-md", "dvfsrc",
};

/* A level interrupt nobody acknowledges starves the CPU it lands on: notice and mask it. */
/*
 * bring-up: log doorbells, messages and a register watch at error level, so that
 * they reach the persistent console when the SoC stops
 */
static bool trace;
module_param(trace, bool, 0444);

#define mtk_md_trace(md, fmt, ...) \
	do { if (trace) dev_err((md)->dev, "trace: " fmt, ##__VA_ARGS__); } while (0)

/* bring-up: leave the CLDMA engine stopped, to see whether the data path is involved */
static bool no_cldma;
module_param(no_cldma, bool, 0444);

/* bring-up: start the SPM firmware and stop there, to see that step on its own */
static bool spm_only;
module_param(spm_only, bool, 0444);

struct mtk_md_irq_guard {
	unsigned long window;
	unsigned int count;
};

#define MTK_MD_IRQ_STORM	1000	/* interrupts within 100 ms */

enum { MTK_MD_IRQ_CLDMA, MTK_MD_IRQ_CCIF0, MTK_MD_IRQ_CCIF1, MTK_MD_IRQ_WDT, MTK_MD_IRQ_NUM };

struct mtk_md {
	struct device *dev;
	struct mtk_md_irq_guard guard[MTK_MD_IRQ_NUM];
	void __iomem *cldma_ao;
	void __iomem *cldma_pd;
	void __iomem *ap_ccif;
	void __iomem *md_ccif;
	void __iomem *md_pll;
	void __iomem *md_clksw;
	void __iomem *md_rgu;
	void __iomem *md_boot;
	struct regmap *scpsys;
	struct regmap *infracfg;
	struct regmap *topckgen;
	struct regmap *apmixed;
	struct regmap *pmic;
	bool powered;
	int irq_cldma, irq_ccif0, irq_ccif1, irq_wdt;
	struct clk_bulk_data clks[ARRAY_SIZE(mtk_md_clk_names)];
	struct reset_control_bulk_data resets[MTK_MD_RST_NUM];
	bool clks_on;

	struct mtk_md_lk_hdr lk;
	void *lk_tags;
	struct mtk_md_lk_modem image;
	struct mtk_md_lk_smem smem;
	phys_addr_t smem_nc;		/* AP/MD1 non-cacheable share memory */
	u32 smem_nc_size;
	u64 ccb_base;			/* cacheable share memory (CCB data, DHL) */
	u32 ccb_size;
	u32 ccb_md_view;
	u32 image_size;			/* what LK loaded, "md1img" */
	u32 c2k_lte_mode;
	u32 ps1_rat;
	u32 ring_total;			/* bytes of normal CCIF queues */

	/* One idle descriptor per CLDMA queue: four TX, then one RX. */
	void *gpd;
	dma_addr_t gpd_dma;

	void *smem_va;			/* the AP/MD1 share memory, mapped for the driver's life */
	struct mtk_md_ring *ring[MTK_MD_RING_QUEUES];
	struct mtk_md_ring *ring_exp[MTK_MD_RING_QUEUES];
	struct work_struct rx_work;
	unsigned long rx_pending;	/* one bit per queue with a doorbell to serve */
	spinlock_t tx_lock;		/* the transmit side of the queues, and tx_seq */
	u16 tx_seq[MTK_MD_TX_SEQ_CHANNELS];
	bool ready;			/* HS2 seen */
	struct miscdevice fs_misc;
	struct sk_buff_head fs_rx;
	wait_queue_head_t fs_wq;
	atomic_t fs_open;
	s32 sim[MTK_MD_SIM_MAX][MTK_MD_SIM_ATTRS];
	bool sim_present[MTK_MD_SIM_MAX];
	unsigned int rx_logged;

	struct work_struct start_work;
	struct completion hs1_done;
	u8 hs1[MTK_MD_HS1_LEN];
};

/* Read what LK loaded. The tag list lives in RAM LK reserved; copy it out and let go. */
static int mtk_md_read_lk(struct mtk_md *md)
{
	struct mtk_md_lk_info info;
	struct device_node *chosen;
	const void *prop;
	void *map, *copy;
	const void *tag;
	u32 v, tag_len;
	int len, ret;

	chosen = of_find_node_by_path("/chosen");
	prop = chosen ? of_get_property(chosen, "ccci,modem_info_v2", &len) : NULL;
	of_node_put(chosen);
	if (!prop)
		return dev_err_probe(md->dev, -ENODEV,
				     "lk: no /chosen/ccci,modem_info_v2, so LK loaded no modem\n");

	ret = mtk_md_lk_parse_hdr(prop, len, &md->lk);
	dev_info(md->dev, "lk: info at %#llx, %u bytes, version %d, %u tags, flag %#x, err %d\n",
		 md->lk.base, md->lk.size, md->lk.version, md->lk.tag_num, md->lk.ld_flag,
		 md->lk.err_no);
	if (ret)
		return dev_err_probe(md->dev, ret, "lk: unusable modem info header\n");

	map = memremap(md->lk.base, md->lk.size, MEMREMAP_WC);
	if (!map)
		return dev_err_probe(md->dev, -ENOMEM, "lk: cannot map the tag list\n");
	copy = devm_kmemdup(md->dev, map, md->lk.size, GFP_KERNEL);
	memunmap(map);
	if (!copy)
		return -ENOMEM;

	info = (struct mtk_md_lk_info) {
		.buf = copy, .size = md->lk.size, .tag_num = md->lk.tag_num,
		.version = md->lk.version,
	};

	if (mtk_md_lk_get_modem(&info, 0, &md->image)) {
		__le32 *stash;

		stash = memremap(md->lk.base + MTK_MD_LK_STASH_OFFSET, SZ_8K, MEMREMAP_WC);
		if (!stash)
			return -ENOMEM;
		if (le32_to_cpu(stash[0]) == MTK_MD_LK_STASH_MAGIC &&
		    le32_to_cpu(stash[1]) == md->lk.size) {
			memcpy(copy, &stash[2], md->lk.size);
			dev_info(md->dev, "lk: tag list taken from the copy of an earlier start\n");
		}
		memunmap(stash);
	}
	md->lk_tags = copy;

	ret = mtk_md_lk_get_modem(&info, 0, &md->image);
	if (ret)
		return dev_err_probe(md->dev, ret, "lk: no modem image entry\n");
	dev_info(md->dev, "lk: md%u image at %#llx, %#x bytes, type %u, errno %d\n",
		 md->image.md_id + 1, md->image.base, md->image.size, md->image.md_type,
		 md->image.err_no);
	if (md->image.err_no)
		return dev_err_probe(md->dev, -EIO, "lk: LK failed to load the modem\n");

	ret = mtk_md_lk_get_smem(&info, &md->smem);
	if (ret)
		return dev_err_probe(md->dev, ret, "lk: no share memory layout\n");
	md->smem_nc = md->smem.base + md->smem.ap_md1_offset;
	md->smem_nc_size = md->smem.ap_md1_size + md->smem.md1_md3_size;
	dev_info(md->dev, "lk: share memory %#llx+%#x, AP/MD1 %pa+%#x (md view %#x)\n",
		 md->smem.base, md->smem.total_size, &md->smem_nc, md->smem_nc_size,
		 mtk_md_smem_md_view(md->smem_nc));

	if (!mtk_md_lk_get_u32(&info, "md1_phy_cap", &v))
		dev_info(md->dev, "lk: md1_phy_cap %#x\n", v);

	ret = mtk_md_lk_find_tag(&info, "ccb_info", &tag, &tag_len);
	if (ret || tag_len < 12)
		return dev_err_probe(md->dev, -EINVAL, "lk: no cacheable share memory\n");
	md->ccb_base = get_unaligned_le64(tag);
	md->ccb_size = get_unaligned_le32(tag + 8);
	if (mtk_md_lk_get_u32(&info, "md1_smem_cahce_offset", &v))
		v = MTK_MD_CACHE_OFFSET_DEF;
	md->ccb_md_view = MTK_MD_BANK4_BASE + v;
	if (md->ccb_size < MTK_MD_DHL_OFFSET + MTK_MD_DHL_SIZE)
		return dev_err_probe(md->dev, -EINVAL, "lk: cacheable share memory too small\n");
	dev_info(md->dev, "lk: cacheable share memory %#llx+%#x (md view %#x)\n",
		 md->ccb_base, md->ccb_size, md->ccb_md_view);

	if (mtk_md_lk_get_u32(&info, "md1img", &md->image_size))
		md->image_size = md->image.size;
	if (mtk_md_lk_get_u32(&info, "opt_c2k_lte_mode", &md->c2k_lte_mode))
		md->c2k_lte_mode = 0;
	if (mtk_md_lk_get_u32(&info, "opt_ps1_rat", &md->ps1_rat))
		md->ps1_rat = 0;

	return 0;
}

static int mtk_md_ccif_send(struct mtk_md *md, unsigned int ch)
{
	if (readl(md->ap_ccif + APCCIF_BUSY) & BIT(ch))
		return -EBUSY;
	writel(BIT(ch), md->ap_ccif + APCCIF_BUSY);
	writel(ch, md->ap_ccif + APCCIF_TCHNUM);
	return 0;
}

/* The CCIF SRAM takes 32-bit accesses only: a byte write replaces the whole word */
static void mtk_md_sram_write(void __iomem *sram, const void *buf, size_t len)
{
	const __le32 *w = buf;
	size_t i;

	for (i = 0; i < len / 4; i++)
		writel(le32_to_cpu(w[i]), sram + 4 * i);
}

static void mtk_md_sram_read(void __iomem *sram, void *buf, size_t len)
{
	__le32 *w = buf;
	size_t i;

	for (i = 0; i < len / 4; i++)
		w[i] = cpu_to_le32(readl(sram + 4 * i));
}

static void mtk_md_sram_clear(void __iomem *sram)
{
	size_t i;

	for (i = 0; i < CCIF_SRAM_SIZE; i += 4)
		writel(0, sram + i);
}

static bool mtk_md_irq_storm(struct mtk_md *md, unsigned int which, int irq)
{
	struct mtk_md_irq_guard *g = &md->guard[which];

	if (time_after(jiffies, g->window + HZ / 10)) {
		g->window = jiffies;
		g->count = 0;
	}
	if (++g->count < MTK_MD_IRQ_STORM)
		return false;

	disable_irq_nosync(irq);
	dev_err(md->dev, "irq %d (line %u) fires without end, masked\n", irq, which);
	return true;
}

/* The data line (md_ccif_isr): ring queues 0-7 and the SRAM mailbox. */
static irqreturn_t mtk_md_ccif_data_irq(int irq, void *data)
{
	struct mtk_md *md = data;
	u32 ch = readl(md->ap_ccif + APCCIF_RCHNUM);

	writel(ch & CCIF_DATA_CHANNELS, md->ap_ccif + APCCIF_ACK);
	mtk_md_trace(md, "ccif data doorbell %#x\n", ch);
	if (mtk_md_irq_storm(md, MTK_MD_IRQ_CCIF0, irq))
		dev_err(md->dev, "ccif data line: RCHNUM %#x\n", ch);

	if (ch & BIT(CCIF_CH_SRAM)) {
		mtk_md_sram_read(md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_DL_HEADER, md->hs1,
				 sizeof(md->hs1));
		complete(&md->hs1_done);
	}
	if (ch & GENMASK(MTK_MD_RING_QUEUES - 1, 0)) {
		unsigned long q, queues = ch & GENMASK(MTK_MD_RING_QUEUES - 1, 0);

		for_each_set_bit(q, &queues, MTK_MD_RING_QUEUES)
			set_bit(q, &md->rx_pending);
		schedule_work(&md->rx_work);
	}

	return IRQ_HANDLED;
}

/* The control line (md_cd_ccif_isr): exception, wakeup and sequence-error channels. */
static irqreturn_t mtk_md_ccif_ctrl_irq(int irq, void *data)
{
	struct mtk_md *md = data;
	u32 ch = readl(md->ap_ccif + APCCIF_RCHNUM);

	writel(ch & CCIF_CTRL_CHANNELS, md->ap_ccif + APCCIF_ACK);
	mtk_md_trace(md, "ccif control doorbell %#x\n", ch);
	if (mtk_md_irq_storm(md, MTK_MD_IRQ_CCIF1, irq))
		dev_err(md->dev, "ccif control line: RCHNUM %#x\n", ch);

	if (ch & BIT(CCIF_CH_EXCEPTION_INIT))
		dev_err(md->dev, "ccif: modem exception (EXCEPTION_INIT)\n");
	if (ch & BIT(CCIF_CH_SEQ_ERROR))
		dev_err(md->dev, "ccif: modem reports a sequence error\n");
	if (ch & BIT(CCIF_CH_PEER_WAKEUP))
		dev_dbg(md->dev, "ccif: modem wakes the AP\n");

	return IRQ_HANDLED;
}

static irqreturn_t mtk_md_wdt_irq(int irq, void *data)
{
	struct mtk_md *md = data;

	mtk_md_trace(md, "modem watchdog irq\n");
	if (!mtk_md_irq_storm(md, MTK_MD_IRQ_WDT, irq))
		dev_err_ratelimited(md->dev, "modem watchdog fired\n");
	return IRQ_HANDLED;
}

/* No queues are in use yet, so this only acknowledges whatever the engine reports. */
static irqreturn_t mtk_md_cldma_irq(int irq, void *data)
{
	struct mtk_md *md = data;
	u32 tx = readl(md->cldma_pd + CLDMA_PD_L2TISAR0);
	u32 rx = readl(md->cldma_pd + CLDMA_PD_L2RISAR0);

	writel(tx, md->cldma_pd + CLDMA_PD_L2TISAR0);
	writel(rx, md->cldma_pd + CLDMA_PD_L2RISAR0);
	mtk_md_trace(md, "cldma irq tx %#x rx %#x\n", tx, rx);
	if (mtk_md_irq_storm(md, MTK_MD_IRQ_CLDMA, irq))
		dev_err(md->dev, "cldma: L2 tx %#x rx %#x, L3 tx %#x %#x rx %#x %#x\n", tx, rx,
			readl(md->cldma_pd + CLDMA_PD_L3TISAR0),
			readl(md->cldma_pd + CLDMA_PD_L3TISAR1),
			readl(md->cldma_pd + CLDMA_PD_L3RISAR0),
			readl(md->cldma_pd + CLDMA_PD_L3RISAR1));
	dev_info_ratelimited(md->dev, "cldma: tx %#x rx %#x, queues not implemented\n", tx, rx);

	return IRQ_HANDLED;
}

/*
 * cldma_write32_ao_misc(): on 6293 the always-on mask set/clear registers are read-write rather
 * than write-one, so clear the same bits in the opposite register before writing this one.
 */
static void mtk_md_cldma_ao_mask(struct mtk_md *md, unsigned int reg, u32 val)
{
	unsigned int other = reg == CLDMA_AO_L2RIMSR0 ? CLDMA_AO_L2RIMCR0 : CLDMA_AO_L2RIMSR0;

	writel(readl(md->cldma_ao + other) & ~val, md->cldma_ao + other);
	writel(val, md->cldma_ao + reg);
}

static void mtk_md_gpd_init(struct mtk_md *md)
{
	struct cldma_tgpd *tgpd = md->gpd;
	struct cldma_rgpd *rgpd = md->gpd + CLDMA_TXQ_NUM * sizeof(*tgpd);
	dma_addr_t rx = md->gpd_dma + CLDMA_TXQ_NUM * sizeof(*tgpd);
	int q;

	/*
	 * STAND-IN: each queue points at one descriptor that links to itself and is not owned by
	 * the hardware (HWO clear), so a started engine finds nothing to do. Real rings replace
	 * this with the data path.
	 */
	for (q = 0; q < CLDMA_TXQ_NUM; q++) {
		dma_addr_t self = md->gpd_dma + q * sizeof(*tgpd);

		tgpd[q].non_used = 1;
		tgpd[q].next_gpd_ptr = cpu_to_le32(lower_32_bits(self));
		tgpd[q].msb = FIELD_PREP(CLDMA_GPD_MSB_NEXT, upper_32_bits(self) & 0xf);
	}
	rgpd->next_gpd_ptr = cpu_to_le32(lower_32_bits(rx));
	rgpd->msb = FIELD_PREP(CLDMA_GPD_MSB_NEXT, upper_32_bits(rx) & 0xf);
}

/* cldma_reset(), 6293 branch */
static void mtk_md_cldma_reset(struct mtk_md *md)
{
	writel(CLDMA_MTU_SIZE, md->cldma_ao + CLDMA_AO_DL_MTU_SIZE);
	writel(readl(md->cldma_ao + CLDMA_AO_SO_CFG) | CLDMA_SO_CFG_ENABLE,
	       md->cldma_ao + CLDMA_AO_SO_CFG);
}

/* cldma_start(): queue addresses, receive on, unmask. Transmit starts on the first send. */
static void mtk_md_cldma_start(struct mtk_md *md)
{
	dma_addr_t rx = md->gpd_dma + CLDMA_TXQ_NUM * sizeof(struct cldma_tgpd);
	u32 msb = 0;
	int q;

	for (q = 0; q < CLDMA_TXQ_NUM; q++) {
		dma_addr_t tx = md->gpd_dma + q * sizeof(struct cldma_tgpd);

		writel(lower_32_bits(tx), md->cldma_pd + CLDMA_PD_UL_START_ADDR(q));
		writel(lower_32_bits(tx), md->cldma_ao + CLDMA_AO_UL_START_ADDR_BK(q));
		msb |= (upper_32_bits(tx) & 0xf) << (4 * q);
	}
	writel(msb, md->cldma_pd + CLDMA_PD_UL_START_ADDR_4MSB);
	writel(msb, md->cldma_ao + CLDMA_AO_UL_START_ADDR_BK_4MSB);
	writel(lower_32_bits(rx), md->cldma_ao + CLDMA_AO_SO_START_ADDR(0));
	writel(upper_32_bits(rx) & 0xf, md->cldma_ao + CLDMA_AO_SO_START_ADDR_4MSB);
	wmb();	/* addresses before the start command, as the vendor */

	writel(CLDMA_ALL_QUEUES, md->cldma_pd + CLDMA_PD_SO_START_CMD);
	readl(md->cldma_pd + CLDMA_PD_SO_START_CMD);

	writel(CLDMA_TX_INT_DONE | CLDMA_TX_INT_QUEUE_EMPTY | CLDMA_TX_INT_ERROR,
	       md->cldma_pd + CLDMA_PD_L2TIMCR0);
	mtk_md_cldma_ao_mask(md, CLDMA_AO_L2RIMCR0,
			     CLDMA_RX_INT_DONE | CLDMA_RX_INT_QUEUE_EMPTY | CLDMA_RX_INT_ERROR);
	writel(~0U, md->cldma_pd + CLDMA_PD_L3TIMCR0);
	writel(~0U, md->cldma_pd + CLDMA_PD_L3TIMCR1);
	writel(~0U, md->cldma_pd + CLDMA_PD_L3RIMCR0);
	writel(~0U, md->cldma_pd + CLDMA_PD_L3RIMCR1);
}

/* md_ccif_sram_reset() and ccci_reset_ccif_hw() */
static void mtk_md_ccif_reset(struct mtk_md *md)
{
	void __iomem *sram = md->ap_ccif + APCCIF_CHDATA;

	reset_control_assert(md->resets[MTK_MD_RST_CCIF].rstc);
	reset_control_deassert(md->resets[MTK_MD_RST_CCIF].rstc);
	mtk_md_sram_clear(sram);
	mtk_md_sram_clear(md->md_ccif + APCCIF_CHDATA);

	/* Where the modem may leave its debug dump: SMEM_USER_RAW_MDSS_DBG, 10 KiB at 2 KiB. */
	writel(CCIF_SRAM_DBG_MAGIC_VAL, sram + CCIF_SRAM_DBG_MAGIC);
	writel(mtk_md_smem_md_view(md->smem_nc + SZ_2K), sram + CCIF_SRAM_DBG_MAGIC + 4);
	writel(10 * SZ_1K, sram + CCIF_SRAM_DBG_MAGIC + 8);
}

/* md_cldma_hw_reset() */
static void mtk_md_cldma_hw_reset(struct mtk_md *md)
{
	reset_control_assert(md->resets[MTK_MD_RST_CLDMA_AO].rstc);
	reset_control_deassert(md->resets[MTK_MD_RST_CLDMA_AO].rstc);
	reset_control_assert(md->resets[MTK_MD_RST_CLDMA_PD].rstc);
	reset_control_deassert(md->resets[MTK_MD_RST_CLDMA_PD].rstc);
	regmap_set_bits(md->infracfg, INFRA_CLDMA_CTRL, INFRA_CLDMA_IP_BUSY_MASK);
}

/* spm_mtcmos_ctrl_md1(STA_POWER_DOWN) */
static void mtk_md_mtcmos_off(struct mtk_md *md)
{
	u32 v;

	regmap_write(md->infracfg, INFRA_PERI2MD_PROT_SET, INFRA_PERI2MD_PROT);
	regmap_read_poll_timeout(md->infracfg, INFRA_PERI2MD_PROT_STA, v, v & INFRA_PERI2MD_PROT,
				 MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	regmap_write(md->infracfg, INFRA_PERI2MD_PROT_SET, INFRA_MD1_PROT);
	regmap_read_poll_timeout(md->infracfg, INFRA_PERI2MD_PROT_STA, v,
				 (v & INFRA_MD1_PROT) == INFRA_MD1_PROT,
				 MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	regmap_write(md->infracfg, INFRA_MD2PERI_PROT_SET, INFRA_MD2PERI_PROT);
	regmap_read_poll_timeout(md->infracfg, INFRA_MD2PERI_PROT_STA, v, v & INFRA_MD2PERI_PROT,
				 MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);

	regmap_set_bits(md->scpsys, SPM_MD_EXTRA_PWR_CON, BIT(0));
	regmap_set_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_CLK_DIS);
	regmap_set_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_ISO);
	regmap_clear_bits(md->scpsys, SPM_MD_SRAM_ISO_CON, BIT(0));
	regmap_set_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_MD1_SRAM_PDN);
	regmap_clear_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_ON);
	regmap_clear_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_ON_2ND);
	regmap_read_poll_timeout(md->scpsys, SPM_PWR_STATUS, v, !(v & SPM_PWR_STATUS_MD1),
				 MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
}

/* spm_mtcmos_ctrl_md1(STA_POWER_ON), in the vendor's order */
static int mtk_md_mtcmos_on(struct mtk_md *md)
{
	u32 v;
	int ret;

	regmap_clear_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_RST_B);
	regmap_set_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_ON);
	regmap_set_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_ON_2ND);
	ret = regmap_read_poll_timeout(md->scpsys, SPM_PWR_STATUS, v, v & SPM_PWR_STATUS_MD1,
				       MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	if (!ret)
		ret = regmap_read_poll_timeout(md->scpsys, SPM_PWR_STATUS_2ND, v,
					       v & SPM_PWR_STATUS_MD1,
					       MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(md->dev, "step 4: MD1 power switch did not acknowledge\n");
		mtk_md_mtcmos_off(md);
		return ret;
	}

	regmap_clear_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_MD1_SRAM_PDN);
	regmap_set_bits(md->scpsys, SPM_MD_SRAM_ISO_CON, BIT(0));
	regmap_clear_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_ISO);
	regmap_clear_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_CLK_DIS);
	regmap_set_bits(md->scpsys, SPM_MD1_PWR_CON, SPM_PWR_RST_B);
	regmap_clear_bits(md->scpsys, SPM_MD_EXTRA_PWR_CON, BIT(0));

	regmap_write(md->infracfg, INFRA_PERI2MD_PROT_CLR, INFRA_MD1_PROT);
	regmap_write(md->infracfg, INFRA_MD2PERI_PROT_CLR, INFRA_MD2PERI_PROT);
	regmap_write(md->infracfg, INFRA_PERI2MD_PROT_CLR, INFRA_PERI2MD_PROT);
	return 0;
}

/* md1_pll_init() */
static int mtk_md_pll_init(struct mtk_md *md)
{
	static const u32 pll_con[] = {
		0x80213c00, 0x80204e00, 0x80229e00, 0x80171400, 0x801713b1,
	};
	unsigned int tries;
	u32 v;
	int i, ret;

	ret = readl_poll_timeout(md->md_pll + MD_PLL_VERSION, v, v, 20000, MTK_MD_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(md->dev, "step 4: the modem's PLL block does not answer\n");
		return ret;
	}
	dev_info(md->dev, "step 4: modem PLL version %#x\n", v);

	regmap_set_bits(md->apmixed, APMIXED_AP_PLL_CON0, APMIXED_CLKSQ1_LPF_EN);
	usleep_range(100, 200);

	writel(0x02020e93, md->md_pll + MD_PLL_SRCLKENA_SETTLE);
	for (i = ARRAY_SIZE(pll_con) - 1; i >= 0; i--)
		writel(pll_con[i], md->md_pll + MD_PLL_CON(i));

	ret = readl_poll_timeout(md->md_pll + MD_PLL_STATUS, v, !(v & MD_PLL_STATUS_BUSY),
				 MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(md->dev, "step 4: modem PLL stays busy\n");
		return ret;
	}
	writel(readl(md->md_pll + MD_PLL_CON_6) & ~BIT(7), md->md_pll + MD_PLL_CON_6);
	writel(0x04c43100, md->md_pll + MD_PLL_DFS);

	/* the vendor rewrites the request every fifth read until it sticks */
	for (tries = 0; tries < 50; tries++) {
		if (!(tries % 5))
			writel(MD_PLL_CLKSW_REQ_VAL, md->md_pll + MD_PLL_CLKSW_REQ);
		msleep(20);
		if (readl(md->md_pll + MD_PLL_CLKSW_REQ) == MD_PLL_CLKSW_REQ_VAL)
			break;
	}
	if (tries == 50) {
		dev_err(md->dev, "step 4: modem clock switch request not taken\n");
		return -ETIMEDOUT;
	}

	ret = readl_poll_timeout(md->md_clksw + MD_CLKSW_STATUS, v, v & MD_CLKSW_STATUS_READY,
				 20000, MTK_MD_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(md->dev, "step 4: modem clock switch not ready (%#x)\n", v);
		return ret;
	}

	writel(readl(md->md_clksw + MD_CLKSW_CKEN) | 0x3, md->md_clksw + MD_CLKSW_CKEN);
	writel(readl(md->md_clksw + MD_CLKSW_CKEN) | 0x058103fc, md->md_clksw + MD_CLKSW_CKEN);
	writel(readl(md->md_clksw + MD_CLKSW_CKEN2) | 0x10, md->md_clksw + MD_CLKSW_CKEN2);
	writel(1, md->md_clksw + MD_CLKSW_CKSEL);
	writel(0xffff, md->md_pll + MD_PLL_INT_MASK0);
	writel(0xffff, md->md_pll + MD_PLL_INT_MASK1);
	writel(MD_PLL_INIT_DONE_VAL, md->md_pll + MD_PLL_INIT_DONE);
	return 0;
}

/*
 * spm_check_status_before_dvfs(): the vendor holds a VCORE request around the modem start, and
 * making that request starts the SPM firmware if it is not running. Without the firmware nothing
 * answers the modem's clock and memory requests and the whole SoC stops when the modem first
 * goes idle. Voltage and memory frequency scaling stay off: only the requests are served.
 */
static int mtk_md_spm_firmware(struct mtk_md *md)
{
	struct arm_smccc_res res;
	u32 v;

	regmap_read(md->scpsys, SPM_PCM_REG15_DATA, &v);
	if (v) {
		dev_info(md->dev, "spm: firmware running (r15 %#x)\n", v);
		return 0;
	}

	arm_smccc_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_SPMFW_IDX, SPMFW_LP4X_2CH_3733, 0,
		      0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_INIT, 0, 0,
		      0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_GO,
		      SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DIS_VCORE_DVS |
		      SPM_FLAG_DIS_VCORE_DFS | SPM_FLAG_DISABLE_MMSYS_DVFS, 0,
		      0, 0, 0, 0, &res);

	if (regmap_read_poll_timeout(md->scpsys, SPM_PCM_REG15_DATA, v, v, MTK_MD_POLL_US,
				     MTK_MD_POLL_TIMEOUT_US)) {
		dev_err(md->dev, "spm: the firmware did not start\n");
		return -ETIMEDOUT;
	}
	dev_info(md->dev, "spm: firmware started (r15 %#x)\n", v);
	return 0;
}

static const struct {
	unsigned int op_en, op_cfg;
} mtk_md_rf_ldos[] = {
	{ MT6358_LDO_VFE28_OP_EN, MT6358_LDO_VFE28_OP_CFG },
	{ MT6358_LDO_VRF18_OP_EN, MT6358_LDO_VRF18_OP_CFG },
	{ MT6358_LDO_VRF12_OP_EN, MT6358_LDO_VRF12_OP_CFG },
};

/* The RF supplies follow the modem's clock request while it runs, and are off otherwise */
static void mtk_md_rf_supplies(struct mtk_md *md, bool modem)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_md_rf_ldos); i++) {
		regmap_write(md->pmic, mtk_md_rf_ldos[i].op_cfg, 0);
		regmap_write(md->pmic, mtk_md_rf_ldos[i].op_en,
			     modem ? MT6358_LDO_OP_HW1 : MT6358_LDO_OP_SW);
	}
}

/*
 * The SIM hot-plug interrupts the modem asks about, from child nodes "sim-hot-plug-<n>". The
 * values are the vendor's (port_rpc.c): the EINT number, polarity and sensitivity from its
 * trigger type, debounce in ms, and three board settings the modem interprets itself.
 */
static void mtk_md_read_sims(struct mtk_md *md)
{
	struct device_node *np;
	unsigned int sim, i;
	char name[20];
	u32 v;

	for (sim = 0; sim < MTK_MD_SIM_MAX; sim++) {
		for (i = 0; i < MTK_MD_SIM_ATTRS; i++)
			md->sim[sim][i] = RPC_ERR_SIM_QUERY_TYPE;
		snprintf(name, sizeof(name), "sim-hot-plug-%u", sim + 1);
		np = of_get_child_by_name(md->dev->of_node, name);
		if (!np)
			continue;
		md->sim_present[sim] = true;
		if (!of_property_read_u32(np, "mediatek,eint", &v))
			md->sim[sim][0] = v;
		if (!of_property_read_u32(np, "mediatek,debounce-us", &v))
			md->sim[sim][1] = v / 1000;
		if (!of_property_read_u32(np, "mediatek,eint-trigger", &v)) {
			/* rising and high are polarity 1; the edges are edge sensitive */
			md->sim[sim][2] = !!(v & (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_LEVEL_HIGH));
			md->sim[sim][3] = !!(v & IRQ_TYPE_EDGE_BOTH);
		}
		if (!of_property_read_u32(np, "mediatek,socket-type", &v))
			md->sim[sim][4] = v;
		if (!of_property_read_u32(np, "mediatek,dedicated", &v))
			md->sim[sim][5] = v;
		if (!of_property_read_u32(np, "mediatek,source-pin", &v))
			md->sim[sim][6] = v;
		of_node_put(np);
		dev_info(md->dev, "sim%u hot plug: eint %d, debounce %d ms, source pin %d\n",
			 sim + 1, md->sim[sim][0], md->sim[sim][1], md->sim[sim][6]);
	}
}

static int mtk_md_get_pmic(struct mtk_md *md)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_parse_phandle(md->dev->of_node, "mediatek,pmic", 0);
	if (!np)
		return dev_err_probe(md->dev, -ENODEV, "no mediatek,pmic\n");
	pdev = of_find_device_by_node(np->parent);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;

	/* the PMIC's registers belong to its bus, the PMIC wrapper */
	md->pmic = dev_get_regmap(&pdev->dev, NULL);
	put_device(&pdev->dev);
	return md->pmic ? 0 : -EPROBE_DEFER;
}

/* md_cd_power_off() */
static void mtk_md_power_off(struct mtk_md *md)
{
	if (!md->powered)
		return;
	mtk_md_mtcmos_off(md);
	mtk_md_rf_supplies(md, false);
	regmap_clear_bits(md->infracfg, INFRA_MD_SRCCLKENA, INFRA_MD_SRCCLKENA_MASK);
	regmap_set_bits(md->topckgen, TOPCKGEN_CLK_MODE, TOPCKGEN_MD_CLK_GATES);
	md->powered = false;
	dev_info(md->dev, "modem powered off\n");
}

/*
 * md_cd_power_on(). vmodem and vsram_others are always on and LK leaves the modem's clock buffer
 * on; the RF rails are handed to the modem's clock request.
 */
static int mtk_md_power_on(struct mtk_md *md)
{
	u32 v;
	int ret;

	regmap_clear_bits(md->topckgen, TOPCKGEN_CLK_MODE, TOPCKGEN_MD_CLK_GATES);
	mtk_md_rf_supplies(md, true);

	ret = mtk_md_mtcmos_on(md);
	if (ret) {
		mtk_md_rf_supplies(md, false);
		return ret;
	}
	md->powered = true;
	regmap_read(md->scpsys, SPM_MD1_PWR_CON, &v);
	dev_info(md->dev, "step 4: MD1 power switch on (MD1_PWR_CON %#x)\n", v);

	/* md1_pre_access_md_reg(): the AP may reach the modem, not the other way round */
	regmap_clear_bits(md->infracfg, INFRA_AP2MD_DUMMY, BIT(0));
	regmap_write(md->infracfg, INFRA_MD2PERI_PROT_SET, INFRA_MD2PERI_PROT);
	ret = regmap_read_poll_timeout(md->infracfg, INFRA_MD2PERI_PROT_STA, v,
				       v & INFRA_MD2PERI_PROT,
				       MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	if (ret) {
		dev_err(md->dev, "step 4: modem-to-AP bus protection not acknowledged\n");
		goto err;
	}

	regmap_update_bits(md->infracfg, INFRA_MD_SRCCLKENA, INFRA_MD_SRCCLKENA_MASK,
			   INFRA_MD_SRCCLKENA_MD1);

	ret = mtk_md_pll_init(md);
	if (ret)
		goto err;
	dev_info(md->dev, "step 4: modem PLL set up\n");

	writel(MD_RGU_WDT_MODE_OFF, md->md_rgu + MD_RGU_WDT_MODE);
	return 0;
err:
	mtk_md_power_off(md);
	return ret;
}

/* md_cd_let_md_go() */
static int mtk_md_let_go(struct mtk_md *md)
{
	u32 v;
	int ret;

	writel(1, md->md_boot + MD_BOOT_VECTOR_EN);
	dev_info(md->dev, "step 4b: boot vector enable reads %#x\n",
		 readl(md->md_boot + MD_BOOT_VECTOR_EN));

	/* md1_post_access_md_reg(): now the modem may reach the AP, not the other way round */
	regmap_write(md->infracfg, INFRA_PERI2MD_PROT_SET, INFRA_PERI2MD_PROT);
	ret = regmap_read_poll_timeout(md->infracfg, INFRA_PERI2MD_PROT_STA, v,
				       v & INFRA_PERI2MD_PROT,
				       MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	if (ret)
		return ret;
	regmap_write(md->infracfg, INFRA_MD2PERI_PROT_CLR, INFRA_MD2PERI_PROT);
	ret = regmap_read_poll_timeout(md->infracfg, INFRA_MD2PERI_PROT_STA, v,
				       !(v & INFRA_MD2PERI_PROT),
				       MTK_MD_POLL_US, MTK_MD_POLL_TIMEOUT_US);
	if (ret)
		return ret;
	regmap_set_bits(md->infracfg, INFRA_AP2MD_DUMMY, BIT(0));
	return 0;
}

/* ccci_md_clear_smem(): the modem expects its share memory zeroed on the first start */
static int mtk_md_clear_smem(struct mtk_md *md)
{
	__le32 *stash = md->smem_va + MTK_MD_LK_STASH_OFFSET;
	void *ccb;

	/* the modem's CCB buffer manager trusts what it finds here: DRAM left as is fails it */
	ccb = memremap(md->ccb_base, md->ccb_size, MEMREMAP_WC);
	if (!ccb)
		return -ENOMEM;
	memset(ccb, 0, md->ccb_size);
	memunmap(ccb);

	memset(md->smem_va, 0, md->smem_nc_size);
	if (md->smem_nc == md->lk.base && md->smem_nc_size >= SZ_1M &&
	    md->lk.size <= SZ_8K - 8) {
		stash[0] = cpu_to_le32(MTK_MD_LK_STASH_MAGIC);
		stash[1] = cpu_to_le32(md->lk.size);
		memcpy(&stash[2], md->lk_tags, md->lk.size);
	}
	return 0;
}

/* md_ccif_ring_buf_init() and md_ccif_exp_ring_buf_init() */
static int mtk_md_rings_init(struct mtk_md *md)
{
	void *buf = md->smem_va + MTK_MD_SMEM_CCISM_OFFSET;
	size_t left = MTK_MD_SMEM_CCISM_SIZE, used;
	int q;

	for (q = 0; q < MTK_MD_RING_QUEUES; q++) {
		md->ring[q] = mtk_md_ring_create(buf, left, mtk_md_ring_rx_size[q],
						 mtk_md_ring_tx_size[q], &used);
		if (!md->ring[q])
			return -ENOSPC;
		buf += used;
		left -= used;
	}
	md->ring_total = MTK_MD_SMEM_CCISM_SIZE - left;

	buf = md->smem_va + MTK_MD_SMEM_CCISM_EXP_OFFSET;
	left = MTK_MD_SMEM_CCISM_EXP_SIZE;
	for (q = 0; q < MTK_MD_RING_QUEUES; q++) {
		md->ring_exp[q] = mtk_md_ring_create(buf, left, mtk_md_ring_exp_size[q],
						     mtk_md_ring_exp_size[q], &used);
		if (!md->ring_exp[q])
			return -ENOSPC;
		buf += used;
		left -= used;
	}
	return 0;
}

/*
 * md_ccif_op_send_skb(): one message into a queue, then its doorbell. The modem checks the
 * sequence per channel; FS and RPC carry the assert bit only until the modem is ready.
 */
static int mtk_md_send(struct mtk_md *md, unsigned int q, void *msg, u32 len)
{
	struct mtk_md_ccci_hdr *h = msg;
	u32 status = le32_to_cpu(h->status);
	u32 ch = FIELD_GET(MTK_MD_CCCI_CHANNEL, status);
	unsigned long flags;
	int ret;

	if (q >= MTK_MD_RING_QUEUES || len < sizeof(*h) || ch >= MTK_MD_TX_SEQ_CHANNELS)
		return -EINVAL;

	spin_lock_irqsave(&md->tx_lock, flags);
	status &= MTK_MD_CCCI_CHANNEL;
	status |= FIELD_PREP(MTK_MD_CCCI_SEQ, md->tx_seq[ch]);
	/* the file and RPC services may be preempted on the modem once it is ready */
	if (!(md->ready && (ch == MTK_MD_CH_RPC_TX || ch == MTK_MD_CH_FS_TX)))
		status |= MTK_MD_CCCI_ASSERT;
	h->status = cpu_to_le32(status);
	ret = mtk_md_ring_tx_write(md->ring[q], msg, len);
	if (!ret) {
		md->tx_seq[ch]++;
		/* a pending doorbell makes the modem drain this queue too (vendor md_ccif_send) */
		if (mtk_md_ccif_send(md, q))
			mtk_md_trace(md, "queue %u doorbell already pending\n", q);
	}
	spin_unlock_irqrestore(&md->tx_lock, flags);
	return ret;
}

/* get_eint_attr_DTSVal(): one attribute of one SIM's hot-plug interrupt */
static s32 mtk_md_rpc_eint_attr(struct mtk_md *md, const struct mtk_md_rpc_req *req, s32 *val)
{
	char name[32];
	u32 type, sim;

	if (req->argc < 3 || !req->arg_len[0] || req->arg_len[2] < 4)
		return RPC_ERR_PARAM;
	type = get_unaligned_le32(req->arg[2]);
	if (type >= MTK_MD_SIM_ATTRS)
		return RPC_ERR_SIM_QUERY_TYPE;

	for (sim = 0; sim < MTK_MD_SIM_MAX; sim++) {
		snprintf(name, sizeof(name), "MD1_SIM%u_HOT_PLUG_EINT", sim + 1);
		if (req->arg_len[0] != strlen(name) + 1 ||
		    memcmp(req->arg[0], name, req->arg_len[0]))
			continue;
		if (!md->sim_present[sim] || md->sim[sim][type] < 0)
			break;
		*val = md->sim[sim][type];
		return 0;
	}
	return RPC_ERR_SIM_QUERY_STRING;
}

/* ccci_rpc_work_helper(): answer what the kernel answers; the modem hears "no op" for the rest */
static void mtk_md_rpc(struct mtk_md *md, const u8 *msg, u32 len)
{
	const struct mtk_md_ccci_hdr *h = (const void *)msg;
	struct mtk_md_rpc_req req;
	u8 dtsi[MTK_MD_DTSI_OUT_LEN];
	const void *arg[2];
	u32 arg_len[2] = { 4, 4 }, argc = 2;
	__le32 w[2] = { };
	s32 ret, val = 0;
	u8 *out;
	int n;

	if (mtk_md_rpc_parse(msg, len, &req)) {
		dev_err(md->dev, "rpc: malformed request (%u bytes)\n", len);
		return;
	}
	arg[0] = &w[0];
	arg[1] = &w[1];

	switch (req.op) {
	case RPC_GET_EINT_ATTR:
		ret = mtk_md_rpc_eint_attr(md, &req, &val);
		w[0] = cpu_to_le32(ret);
		w[1] = cpu_to_le32(ret ? ret : val);
		dev_info(md->dev, "rpc: %.*s attribute %u: %d (%d)\n", (int)req.arg_len[0],
			 req.arg[0], req.argc > 2 && req.arg_len[2] >= 4 ?
			 get_unaligned_le32(req.arg[2]) : 0, val, ret);
		break;
	case RPC_DTSI_QUERY:
		/* no board attributes for the modem: 0x0f everywhere is "not set" */
		memset(dtsi, 0x0f, sizeof(dtsi));
		arg[1] = dtsi;
		arg_len[1] = sizeof(dtsi);
		n = req.argc && req.arg_len[0] > 2 ? min_t(u32, req.arg_len[0] - 2, 64) : 0;
		dev_info(md->dev, "rpc: board attribute %.*s: not set\n", n,
			 n ? (const char *)req.arg[0] + 2 : "");
		break;
	case RPC_TRNG:
		w[1] = cpu_to_le32(get_random_u32());
		break;
	case RPC_LHIF_MAPPING:
		break;
	case RPC_GET_TDD_EINT_NUM:
	case RPC_GET_GPIO_NUM:
	case RPC_GET_ADC_NUM:
		/* no GPIO or ADC is lent to the modem on this board */
		w[0] = cpu_to_le32(RPC_ERR_FUNC_FAIL);
		w[1] = w[0];
		dev_info(md->dev, "rpc: %#x for %.*s: none\n", req.op,
			 req.argc ? (int)min_t(u32, req.arg_len[0], 64) : 0,
			 req.argc ? (const char *)req.arg[0] : "");
		break;
	case RPC_GET_GPIO_VAL:
	case RPC_GET_ADC_VAL:
		w[0] = cpu_to_le32(RPC_ERR_FUNC_FAIL);
		w[1] = w[0];
		break;
	default:
		dev_warn(md->dev, "rpc: unhandled operation %#x, %u parameters\n",
			 req.op, req.argc);
		w[0] = cpu_to_le32(RPC_ERR_NO_OP);
		argc = 1;
		break;
	}

	out = kzalloc(MTK_MD_RPC_MAX_LEN, GFP_KERNEL);
	if (!out)
		return;
	n = mtk_md_rpc_build(out, MTK_MD_RPC_MAX_LEN, h, req.op, argc, arg, arg_len);
	if (n > 0)
		n = mtk_md_send(md, MTK_MD_RPC_TXQ, out, n);
	if (n < 0)
		dev_err(md->dev, "rpc: answer to %#x not sent: %d\n", req.op, n);
	kfree(out);
}

static void mtk_md_fs_rx(struct mtk_md *md, const u8 *msg, u32 len)
{
	struct sk_buff *skb;

	if (!atomic_read(&md->fs_open)) {
		dev_warn_ratelimited(md->dev, "fs: request with no file service running\n");
		return;
	}
	if (skb_queue_len(&md->fs_rx) >= MTK_MD_FS_RX_BACKLOG) {
		dev_err_ratelimited(md->dev, "fs: file service not keeping up, request dropped\n");
		return;
	}
	skb = alloc_skb(len, GFP_KERNEL);
	if (!skb)
		return;
	skb_put_data(skb, msg, len);
	skb_queue_tail(&md->fs_rx, skb);
	wake_up_interruptible(&md->fs_wq);
}

static struct mtk_md *mtk_md_fs_md(struct file *file)
{
	return container_of(file->private_data, struct mtk_md, fs_misc);
}

static int mtk_md_fs_open(struct inode *inode, struct file *file)
{
	struct mtk_md *md = mtk_md_fs_md(file);

	/* one file service: the modem's requests have no reader address */
	if (atomic_cmpxchg(&md->fs_open, 0, 1))
		return -EBUSY;
	skb_queue_purge(&md->fs_rx);
	return 0;
}

static int mtk_md_fs_release(struct inode *inode, struct file *file)
{
	struct mtk_md *md = mtk_md_fs_md(file);

	atomic_set(&md->fs_open, 0);
	skb_queue_purge(&md->fs_rx);
	return 0;
}

static ssize_t mtk_md_fs_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct mtk_md *md = mtk_md_fs_md(file);
	struct sk_buff *skb;
	ssize_t ret;

	for (;;) {
		skb = skb_dequeue(&md->fs_rx);
		if (skb)
			break;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(md->fs_wq, !skb_queue_empty(&md->fs_rx));
		if (ret)
			return ret;
	}

	if (count < skb->len) {
		skb_queue_head(&md->fs_rx, skb);
		return -EMSGSIZE;
	}
	ret = copy_to_user(buf, skb->data, skb->len) ? -EFAULT : skb->len;
	kfree_skb(skb);
	return ret;
}

static ssize_t mtk_md_fs_write(struct file *file, const char __user *buf, size_t count,
			       loff_t *ppos)
{
	struct mtk_md *md = mtk_md_fs_md(file);
	struct mtk_md_ccci_hdr *h;
	void *msg;
	int ret;

	if (count < sizeof(*h) + 4 || count > MTK_MD_FS_MAX_LEN)
		return -EINVAL;
	msg = memdup_user(buf, count);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	/* user space answers on the file channel only; length and channel are ours to set */
	h = msg;
	h->data[1] = cpu_to_le32(count);
	h->status = cpu_to_le32(FIELD_PREP(MTK_MD_CCCI_CHANNEL, MTK_MD_CH_FS_TX));
	ret = mtk_md_send(md, MTK_MD_FS_Q, msg, count);
	kfree(msg);
	return ret ? ret : count;
}

static __poll_t mtk_md_fs_poll(struct file *file, poll_table *wait)
{
	struct mtk_md *md = mtk_md_fs_md(file);

	poll_wait(file, &md->fs_wq, wait);
	return (skb_queue_empty(&md->fs_rx) ? 0 : EPOLLIN | EPOLLRDNORM) | EPOLLOUT | EPOLLWRNORM;
}

static const struct file_operations mtk_md_fs_fops = {
	.owner = THIS_MODULE,
	.open = mtk_md_fs_open,
	.release = mtk_md_fs_release,
	.read = mtk_md_fs_read,
	.write = mtk_md_fs_write,
	.poll = mtk_md_fs_poll,
	.llseek = noop_llseek,
};

static void mtk_md_fs_unregister(void *data)
{
	struct mtk_md *md = data;

	misc_deregister(&md->fs_misc);
	skb_queue_purge(&md->fs_rx);
}

static int mtk_md_fs_register(struct mtk_md *md)
{
	int ret;

	skb_queue_head_init(&md->fs_rx);
	init_waitqueue_head(&md->fs_wq);
	md->fs_misc.minor = MISC_DYNAMIC_MINOR;
	md->fs_misc.name = "mtk_md_fs";
	md->fs_misc.fops = &mtk_md_fs_fops;
	md->fs_misc.parent = md->dev;
	ret = misc_register(&md->fs_misc);
	if (ret)
		return ret;
	return devm_add_action_or_reset(md->dev, mtk_md_fs_unregister, md);
}

static void mtk_md_rx_one(struct mtk_md *md, unsigned int q, const u8 *msg, u32 len)
{
	const struct mtk_md_ccci_hdr *h = (const void *)msg;
	u32 status = le32_to_cpu(h->status);
	u32 ch = FIELD_GET(MTK_MD_CCCI_CHANNEL, status);

	if (ch == MTK_MD_CH_FS_RX) {
		mtk_md_fs_rx(md, msg, len);
		return;
	}

	if (ch == MTK_MD_CH_RPC_RX) {
		mtk_md_trace(md, "rx q%u rpc len %u\n", q, len);
		mtk_md_rpc(md, msg, len);
		return;
	}

	if (ch == MTK_MD_CH_CONTROL_RX) {
		switch (mtk_md_ctrl_classify(h)) {
		case MTK_MD_CTRL_HS2:
			md->ready = true;
			dev_info(md->dev, "step 13: HS2, md_state 3 -> 4: the modem is ready\n");
			return;
		case MTK_MD_CTRL_EXCEPTION:
			dev_err(md->dev, "control: the modem reports an exception\n");
			return;
		default:
			break;
		}
	}

	mtk_md_trace(md, "rx q%u ch %u len %u: %08x %08x %08x | %*ph\n", q, ch, len,
		     le32_to_cpu(h->data[0]), le32_to_cpu(h->data[1]), le32_to_cpu(h->reserved),
		     (int)min_t(u32, len - sizeof(*h), 16), msg + sizeof(*h));
	if (md->rx_logged < MTK_MD_RX_LOG_LIMIT) {
		md->rx_logged++;
		dev_info(md->dev, "rx q%u ch %u seq %lu len %u: %08x %08x %08x | %*ph\n", q, ch,
			 FIELD_GET(MTK_MD_CCCI_SEQ, status), len, le32_to_cpu(h->data[0]),
			 le32_to_cpu(h->data[1]), le32_to_cpu(h->reserved),
			 (int)min_t(u32, len - sizeof(*h), 32), msg + sizeof(*h));
	}
}

/* ccif_rx_collect() */
static void mtk_md_rx_work(struct work_struct *work)
{
	struct mtk_md *md = container_of(work, struct mtk_md, rx_work);
	unsigned int q;
	u8 *msg;
	int len;

	for_each_set_bit(q, &md->rx_pending, MTK_MD_RING_QUEUES) {
		clear_bit(q, &md->rx_pending);
		while ((len = mtk_md_ring_rx_peek(md->ring[q])) > 0) {
			msg = kmalloc(len, GFP_KERNEL);
			if (!msg)
				return;
			mtk_md_ring_rx_read(md->ring[q], msg, len);
			mtk_md_rx_one(md, q, msg, len);
			kfree(msg);
			mtk_md_ring_rx_consume(md->ring[q], len);
		}
		if (len == -EBADMSG)
			dev_err_ratelimited(md->dev, "rx q%u: not a message (read %u write %u)\n",
					    q, le32_to_cpu(md->ring[q]->rx_read),
					    le32_to_cpu(md->ring[q]->rx_write));
	}
}

enum mtk_md_rt_kind {
	RT_SKIP,	/* the vendor has no case: nothing is appended */
	RT_EMPTY,	/* header only */
	RT_BOOT,
	RT_IMAGE,	/* {AP physical address, size} of the image LK loaded */
	RT_DHL,		/* {md view address, size} in the cacheable share memory */
	RT_SHM,		/* {md view address, size} in the non-cacheable share memory */
	RT_SHM_ZERO,	/* {0, 0} */
	RT_MISC,	/* four words */
	RT_U32,
};

struct mtk_md_rt_src {
	u8 kind;
	bool stand_in;	/* the value sent is not the one the vendor would send */
	u32 off;
	u32 size;
};

/* eccci/ccci_modem.c:md1_6293_noncacheable_fat[] and ccci_md_prepare_runtime_data() */
static const struct mtk_md_rt_src mtk_md_rt_srcs[MTK_MD_FEATURE_COUNT] = {
	[MTK_MD_RT_BOOT_INFO]			= { RT_BOOT },
	[MTK_MD_RT_EXCEPTION_SHARE_MEMORY]	= { RT_SHM, false, 0, SZ_64K },
	[MTK_MD_RT_CCIF_SHARE_MEMORY]		= { RT_SHM, false, 160 * SZ_1K, 705 * SZ_1K },
	[MTK_MD_RT_SMART_LOGGING_SHARE_MEMORY]	= { RT_SHM_ZERO, true },
	[MTK_MD_RT_MD1MD3_SHARE_MEMORY]		= { RT_SHM_ZERO, true },
	[MTK_MD_RT_MISC_INFO_HIF_DMA_REMAP]	= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_RTC_32K_LESS]	= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_RANDOM_SEED_NUM]	= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_GPS_COCLOCK]	= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_SBP_ID]		= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_CCCI]		= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_CLIB_TIME]		= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_C2K]		= { RT_MISC },
	[MTK_MD_RT_MD_IMAGE_START_MEMORY]	= { RT_IMAGE },
	[MTK_MD_RT_CCISM_SHARE_MEMORY]		= { RT_SHM, false, 64 * SZ_1K, 32 * SZ_1K },
	[MTK_MD_RT_CCB_SHARE_MEMORY]		= { RT_MISC },
	[MTK_MD_RT_DHL_RAW_SHARE_MEMORY]	= { RT_DHL },
	[MTK_MD_RT_DT_NETD_SHARE_MEMORY]	= { RT_SHM, false, 100 * SZ_1K, 4 * SZ_1K },
	[MTK_MD_RT_DT_USB_SHARE_MEMORY]		= { RT_SHM, false, 104 * SZ_1K, 4 * SZ_1K },
	[MTK_MD_RT_EE_AFTER_EPOF]		= { RT_MISC },
	[MTK_MD_RT_AP_CCMNI_MTU]		= { RT_U32 },
	[MTK_MD_RT_CCCI_FAST_HEADER]		= { RT_U32 },
	[MTK_MD_RT_LWA_SHARE_MEMORY]		= { RT_SHM, true, SZ_1M, 0 },
	[MTK_MD_RT_AUDIO_RAW_SHARE_MEMORY]	= { RT_SHM, false, 108 * SZ_1K, 52 * SZ_1K },
	[MTK_MD_RT_MULTI_MD_MPU]		= { RT_EMPTY },
	[MTK_MD_RT_CCISM_SHARE_MEMORY_EXP]	= { RT_SHM, false, 865 * SZ_1K, 121 * SZ_1K },
	[MTK_MD_RT_MD_PHY_CAPTURE]		= { RT_SHM, true, SZ_1M, 0 },
	[MTK_MD_RT_MD_CONSYS_SHARE_MEMORY]	= { RT_SHM, true, SZ_1M, 0 },
	[MTK_MD_RT_MD_MTEE_SMEM_ENABLE]		= { RT_U32 },
};

static int mtk_md_rt_one(struct mtk_md *md, u8 *buf, size_t size, size_t *pos, unsigned int id,
			 u8 support)
{
	const struct mtk_md_rt_src *src = &mtk_md_rt_srcs[id];
	struct timespec64 now;
	__le32 w[4] = { };
	u32 len = 0;

	switch (src->kind) {
	case RT_SKIP:
		dev_warn(md->dev, "rt: feature %u accepted, no payload (as the vendor)\n", id);
		return 0;
	case RT_EMPTY:
		break;
	case RT_BOOT:
		w[0] = cpu_to_le32(MTK_MD_CH_CONTROL_RX);
		w[1] = cpu_to_le32(MTK_MD_BOOTING_START_ID);
		len = 16;
		break;
	case RT_IMAGE:
		w[0] = cpu_to_le32(lower_32_bits(md->image.base));
		w[1] = cpu_to_le32(md->image_size);
		len = 8;
		break;
	case RT_DHL:
		w[0] = cpu_to_le32(md->ccb_md_view + MTK_MD_DHL_OFFSET);
		w[1] = cpu_to_le32(MTK_MD_DHL_SIZE);
		len = 8;
		break;
	case RT_SHM:
		w[0] = cpu_to_le32(mtk_md_smem_md_view(md->smem_nc + src->off));
		/* the normal queues: what they take, not the whole region */
		w[1] = cpu_to_le32(id == MTK_MD_RT_CCIF_SHARE_MEMORY ? md->ring_total : src->size);
		len = 8;
		break;
	case RT_SHM_ZERO:
		len = 8;
		break;
	case RT_MISC:
		len = 16;
		if (id == MTK_MD_RT_MISC_INFO_RANDOM_SEED_NUM) {
			w[0] = cpu_to_le32(get_random_u32());
		} else if (id == MTK_MD_RT_MISC_INFO_CCCI) {
			/* sequence check, modem status polling */
			w[0] = cpu_to_le32(BIT(0) | BIT(1));
		} else if (id == MTK_MD_RT_MISC_INFO_CLIB_TIME) {
			ktime_get_real_ts64(&now);
			w[0] = cpu_to_le32(lower_32_bits(now.tv_sec));
			w[1] = cpu_to_le32(upper_32_bits(now.tv_sec));
		} else if (id == MTK_MD_RT_CCB_SHARE_MEMORY) {
			/* control in the non-cacheable share memory, data at the cacheable start */
			w[0] = cpu_to_le32(mtk_md_smem_md_view(md->smem_nc + 96 * SZ_1K));
			w[1] = cpu_to_le32(4 * SZ_1K);
			w[2] = cpu_to_le32(md->ccb_md_view);
			w[3] = cpu_to_le32(MTK_MD_CCB_DATA_SIZE);
		} else if (id == MTK_MD_RT_MISC_INFO_SBP_ID) {
			/* sbp code 0; the world modes the binary supports, as LK reports them */
			w[1] = cpu_to_le32(md->ps1_rat);
		} else if (id == MTK_MD_RT_MISC_INFO_C2K) {
			/* opt_c2k_lte_mode 1: SVLTE, 2: SRLTE */
			if (md->c2k_lte_mode == 1)
				w[0] = cpu_to_le32(BIT(1));
			else if (md->c2k_lte_mode == 2)
				w[0] = cpu_to_le32(BIT(2));
		}
		break;
	case RT_U32:
		len = 4;
		if (id == MTK_MD_RT_AP_CCMNI_MTU)
			w[0] = cpu_to_le32(MTK_MD_CCMNI_MTU);
		else if (id == MTK_MD_RT_CCCI_FAST_HEADER)
			w[0] = cpu_to_le32(1);
		break;
	}

	if (src->stand_in)
		dev_warn(md->dev, "rt: feature %u: STAND-IN payload\n", id);
	return mtk_md_rt_append(buf, size, pos, id, support, w, len);
}

/* ccci_md_prepare_runtime_data(): the TLVs, into the AP half of the runtime data region. */
static int mtk_md_write_runtime_data(struct mtk_md *md, const u8 *negotiated, size_t *total)
{
	size_t pos = 0;
	unsigned int i;
	u8 *buf;
	int ret = 0;

	buf = kzalloc(MTK_MD_SMEM_RUNTIME_AP_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < MTK_MD_FEATURE_COUNT && !ret; i++) {
		if (FIELD_GET(MTK_MD_FEATURE_MASK, negotiated[i]) == MTK_MD_FEATURE_MUST)
			ret = mtk_md_rt_one(md, buf, MTK_MD_SMEM_RUNTIME_AP_SIZE, &pos, i,
					    negotiated[i]);
		else
			ret = mtk_md_rt_append(buf, MTK_MD_SMEM_RUNTIME_AP_SIZE, &pos, i,
					       negotiated[i], NULL, 0);
	}
	if (ret)
		goto out;

	memcpy(md->smem_va + MTK_MD_SMEM_RUNTIME_OFFSET, buf, pos);
	if (trace)
		print_hex_dump(KERN_ERR, "mtk_md rt: ", DUMP_PREFIX_OFFSET, 16, 4, buf, pos, false);
	*total = pos;
out:
	kfree(buf);
	return ret;
}

static int mtk_md_handshake(struct mtk_md *md)
{
	const struct mtk_md_ccci_hdr *h = (const void *)md->hs1;
	u32 rt_md = mtk_md_smem_md_view(md->smem_nc + MTK_MD_SMEM_RUNTIME_OFFSET);
	u8 negotiated[MTK_MD_FEATURE_COUNT];
	const struct mtk_md_md_query *q;
	struct mtk_md_ap_query aq;
	struct mtk_md_ccci_hdr up = { };
	__le32 dbm[MTK_MD_DBM_WORDS];
	void __iomem *sram;
	unsigned int bad;
	size_t len;
	int ret;

	dev_info(md->dev, "step 9: waiting up to %d ms for HS1 in CCIF SRAM\n",
		 MTK_MD_HS_TIMEOUT_MS);
	if (!wait_for_completion_timeout(&md->hs1_done,
					 msecs_to_jiffies(MTK_MD_HS_TIMEOUT_MS))) {
		dev_err(md->dev, "step 9: no HS1 from the modem\n");
		return -ETIMEDOUT;
	}

	dev_info(md->dev, "step 10: control message %#x,%#x on channel %lu\n",
		 le32_to_cpu(h->data[1]), le32_to_cpu(h->reserved),
		 FIELD_GET(MTK_MD_CCCI_CHANNEL, le32_to_cpu(h->status)));
	ret = mtk_md_hs1_check(md->hs1, sizeof(md->hs1), &q);
	if (ret) {
		dev_err(md->dev, "step 10: not an HS1 with a feature query\n");
		return ret;
	}
	dev_info(md->dev, "step 10: HS1, md_state 2 -> 3; features %*phN\n",
		 MTK_MD_FEATURE_COUNT, q->feature_set);

	ret = mtk_md_rt_negotiate(q->feature_set, mtk_md_ap_features_6293, negotiated, &bad);
	if (ret) {
		dev_err(md->dev, "step 11: modem requires feature %u, the AP lacks it\n", bad);
		return ret;
	}

	ret = mtk_md_write_runtime_data(md, negotiated, &len);
	if (ret)
		return ret;
	dev_info(md->dev, "step 11: %zu bytes of runtime data at md view %#x\n", len, rt_md);

	mtk_md_ap_query_fill(&aq, rt_md, mtk_md_smem_md_view(md->smem_nc), md->smem_nc_size,
			     md->ccb_md_view, md->ccb_size);
	up.data[1] = cpu_to_le32(sizeof(up) + sizeof(aq));
	up.status = cpu_to_le32(FIELD_PREP(MTK_MD_CCCI_CHANNEL, MTK_MD_CH_CONTROL_TX));
	up.reserved = cpu_to_le32(MTK_MD_INIT_CHK_ID);
	sram = md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_UP_HEADER;
	mtk_md_sram_write(sram, &up, sizeof(up));
	mtk_md_sram_write(sram + sizeof(up), &aq, sizeof(aq));

	/* the modem checks its power budget block once NVRAM is loaded; vendor fills it here */
	mtk_md_dbm_fill(dbm);
	memcpy(md->smem_va + MTK_MD_SMEM_DBM_OFFSET, dbm, sizeof(dbm));

	ret = mtk_md_ccif_send(md, CCIF_CH_SRAM);
	if (ret) {
		dev_err(md->dev, "step 12: CCIF SRAM channel busy\n");
		return ret;
	}
	dev_info(md->dev, "step 12: runtime data sent\n");
	mtk_md_trace(md, "runtime data sent\n");
	return 0;
}

/* What the SoC does after the modem takes over: print each value when it changes */
static void mtk_md_watch(struct mtk_md *md, unsigned int ms)
{
	static const struct {
		const char *name;
		int blk;	/* 0 spm, 1 infracfg */
		unsigned int off;
	} regs[] = {
		{ "spm_r13_req_in", 0, 0x134 },
		{ "spm_r15", 0, 0x13c },
		{ "spm_fsm", 0, 0x178 },
		{ "spm_src_req_sta", 0, 0x17c },
		{ "spm_pwr_sta", 0, 0x180 },
		{ "spm_src_rdy", 0, 0x194 },
		{ "spm_r12_wake", 0, 0x130 },
		{ "md1_pwr_con", 0, 0x320 },
		{ "infra_prot_sta1", 1, 0x228 },
		{ "infra_prot_sta1_1", 1, 0x258 },
		{ "infra_md_srcclkena", 1, 0xf0c },
	};
	u32 last[ARRAY_SIZE(regs)] = { }, v;
	ktime_t t0 = ktime_get(), end = ktime_add_ms(t0, ms), hb = t0;
	unsigned int i, n = 0, rounds = 0, spin;
	bool first = true;

	/*
	 * Busy-wait instead of sleeping, and count the rounds: if the rounds advance while the
	 * clock stands still, the timer's reference clock has stopped.
	 */
	while (ktime_before(ktime_get(), end) && rounds < 200000000) {
		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			regmap_read(regs[i].blk ? md->infracfg : md->scpsys, regs[i].off, &v);
			if (first || v != last[i]) {
				mtk_md_trace(md, "+%lldus %s %#x\n",
					     ktime_us_delta(ktime_get(), t0), regs[i].name, v);
				last[i] = v;
			}
		}
		v = readl(md->ap_ccif + APCCIF_RCHNUM);
		if (first || v != n) {
			mtk_md_trace(md, "+%lldus ccif_rchnum %#x q0_rx_w %u\n",
				     ktime_us_delta(ktime_get(), t0), v,
				     le32_to_cpu(md->ring[0]->rx_write));
			n = v;
		}
		first = false;
		for (spin = 0; spin < 2000; spin++)
			cpu_relax();
		rounds++;
		if (ktime_us_delta(ktime_get(), hb) >= 1000 || !(rounds % 4096)) {
			mtk_md_trace(md, "hb round %u +%lldus\n", rounds,
				     ktime_us_delta(ktime_get(), t0));
			hb = ktime_get();
		}
	}
	mtk_md_trace(md, "watch done\n");
}

static void mtk_md_start(struct work_struct *work)
{
	struct mtk_md *md = container_of(work, struct mtk_md, start_work);
	int ret;

	if (mtk_md_spm_firmware(md) || spm_only)
		return;

	dev_info(md->dev, "step 1: clocks on\n");
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(md->clks), md->clks);
	if (ret) {
		dev_err(md->dev, "step 1: clocks: %d\n", ret);
		return;
	}
	md->clks_on = true;

	ret = mtk_md_clear_smem(md);
	if (ret) {
		dev_err(md->dev, "cacheable share memory: %d\n", ret);
		return;
	}
	ret = mtk_md_rings_init(md);
	if (ret) {
		dev_err(md->dev, "ring queues do not fit the share memory\n");
		return;
	}

	dev_info(md->dev, "step 2: CCIF reset, SRAM cleared\n");
	mtk_md_ccif_reset(md);

	dev_info(md->dev, "step 3: CLDMA reset\n");
	mtk_md_cldma_hw_reset(md);

	ret = mtk_md_power_on(md);
	if (ret)
		return;

	ret = mtk_md_let_go(md);
	if (ret) {
		dev_err(md->dev, "step 4b: releasing the modem: %d\n", ret);
		mtk_md_power_off(md);
		return;
	}

	dev_info(md->dev, "step 5: interrupts on\n");
	enable_irq(md->irq_wdt);
	enable_irq(md->irq_ccif1);
	enable_irq(md->irq_ccif0);
	enable_irq(md->irq_cldma);

	if (no_cldma) {
		mtk_md_trace(md, "cldma left stopped\n");
	} else {
		dev_info(md->dev, "step 6: cldma_reset (MTU %#x, SO_CFG enable)\n", CLDMA_MTU_SIZE);
		mtk_md_cldma_reset(md);

		dev_info(md->dev, "step 7: cldma_start (idle descriptors at %pad)\n", &md->gpd_dma);
		mtk_md_cldma_start(md);
	}

	dev_info(md->dev, "step 8: started, modem booting\n");
	ret = mtk_md_handshake(md);
	if (ret)
		dev_err(md->dev, "handshake failed: %d\n", ret);
	else if (trace)
		mtk_md_watch(md, 2000);
}

static void mtk_md_teardown(void *data)
{
	struct mtk_md *md = data;

	cancel_work_sync(&md->start_work);
	disable_irq(md->irq_cldma);
	disable_irq(md->irq_ccif0);
	disable_irq(md->irq_ccif1);
	disable_irq(md->irq_wdt);
	cancel_work_sync(&md->rx_work);
	mtk_md_power_off(md);
	if (md->clks_on)
		clk_bulk_disable_unprepare(ARRAY_SIZE(md->clks), md->clks);
}

static int mtk_md_request_irq(struct mtk_md *md, const char *name, irq_handler_t fn, int *irq)
{
	struct platform_device *pdev = to_platform_device(md->dev);
	int ret;

	*irq = platform_get_irq_byname(pdev, name);
	if (*irq < 0)
		return *irq;

	/* off until the modem is released, as the vendor does */
	ret = devm_request_irq(md->dev, *irq, fn, IRQF_NO_AUTOEN, dev_name(md->dev), md);
	if (ret)
		return dev_err_probe(md->dev, ret, "irq %s\n", name);
	/* bring-up: keep a runaway line off the CPU that serves everything else */
	irq_set_affinity(*irq, cpumask_of(cpumask_last(cpu_online_mask)));
	dev_info(md->dev, "irq %s: %d\n", name, *irq);
	return 0;
}

static int mtk_md_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_md *md;
	int i, ret;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->dev = dev;
	init_completion(&md->hs1_done);
	INIT_WORK(&md->start_work, mtk_md_start);
	INIT_WORK(&md->rx_work, mtk_md_rx_work);
	spin_lock_init(&md->tx_lock);

	md->cldma_ao = devm_platform_ioremap_resource_byname(pdev, "cldma-ao");
	if (IS_ERR(md->cldma_ao))
		return PTR_ERR(md->cldma_ao);
	md->cldma_pd = devm_platform_ioremap_resource_byname(pdev, "cldma-pd");
	if (IS_ERR(md->cldma_pd))
		return PTR_ERR(md->cldma_pd);
	md->ap_ccif = devm_platform_ioremap_resource_byname(pdev, "ap-ccif");
	if (IS_ERR(md->ap_ccif))
		return PTR_ERR(md->ap_ccif);
	md->md_ccif = devm_platform_ioremap_resource_byname(pdev, "md-ccif");
	if (IS_ERR(md->md_ccif))
		return PTR_ERR(md->md_ccif);
	md->md_pll = devm_platform_ioremap_resource_byname(pdev, "md-pll");
	if (IS_ERR(md->md_pll))
		return PTR_ERR(md->md_pll);
	md->md_clksw = devm_platform_ioremap_resource_byname(pdev, "md-clksw");
	if (IS_ERR(md->md_clksw))
		return PTR_ERR(md->md_clksw);
	md->md_rgu = devm_platform_ioremap_resource_byname(pdev, "md-rgu");
	if (IS_ERR(md->md_rgu))
		return PTR_ERR(md->md_rgu);
	md->md_boot = devm_platform_ioremap_resource_byname(pdev, "md-boot");
	if (IS_ERR(md->md_boot))
		return PTR_ERR(md->md_boot);

	md->scpsys = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,scpsys");
	if (IS_ERR(md->scpsys))
		return dev_err_probe(dev, PTR_ERR(md->scpsys), "scpsys\n");
	md->infracfg = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(md->infracfg))
		return dev_err_probe(dev, PTR_ERR(md->infracfg), "infracfg\n");
	md->topckgen = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,topckgen");
	if (IS_ERR(md->topckgen))
		return dev_err_probe(dev, PTR_ERR(md->topckgen), "topckgen\n");
	md->apmixed = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,apmixedsys");
	if (IS_ERR(md->apmixed))
		return dev_err_probe(dev, PTR_ERR(md->apmixed), "apmixedsys\n");
	ret = mtk_md_get_pmic(md);
	if (ret)
		return ret;
	mtk_md_read_sims(md);

	for (i = 0; i < ARRAY_SIZE(md->clks); i++)
		md->clks[i].id = mtk_md_clk_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(md->clks), md->clks);
	if (ret)
		return dev_err_probe(dev, ret, "clocks\n");

	md->resets[MTK_MD_RST_CLDMA_AO].id = "cldma-ao";
	md->resets[MTK_MD_RST_CLDMA_PD].id = "cldma-pd";
	md->resets[MTK_MD_RST_CCIF].id = "ccif";
	ret = devm_reset_control_bulk_get_optional_exclusive(dev, MTK_MD_RST_NUM, md->resets);
	if (ret)
		return dev_err_probe(dev, ret, "resets\n");

	ret = mtk_md_request_irq(md, "cldma", mtk_md_cldma_irq, &md->irq_cldma);
	if (!ret)
		ret = mtk_md_request_irq(md, "ccif0", mtk_md_ccif_data_irq, &md->irq_ccif0);
	if (!ret)
		ret = mtk_md_request_irq(md, "ccif1", mtk_md_ccif_ctrl_irq, &md->irq_ccif1);
	if (!ret)
		ret = mtk_md_request_irq(md, "wdt", mtk_md_wdt_irq, &md->irq_wdt);
	if (ret)
		return ret;

	/* CLDMA descriptors carry 36-bit addresses */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	if (ret)
		return dev_err_probe(dev, ret, "dma mask\n");
	md->gpd = dmam_alloc_coherent(dev, (CLDMA_TXQ_NUM + CLDMA_RXQ_NUM) * 16, &md->gpd_dma,
				      GFP_KERNEL);
	if (!md->gpd)
		return -ENOMEM;
	mtk_md_gpd_init(md);

	ret = mtk_md_read_lk(md);
	if (ret)
		return ret;

	if (md->smem_nc_size < SZ_1M)
		return dev_err_probe(dev, -EINVAL, "share memory smaller than the 6293 layout\n");
	md->smem_va = devm_memremap(dev, md->smem_nc, md->smem_nc_size, MEMREMAP_WC);
	if (IS_ERR(md->smem_va))
		return dev_err_probe(dev, PTR_ERR(md->smem_va), "share memory\n");

	ret = mtk_md_fs_register(md);
	if (ret)
		return dev_err_probe(dev, ret, "file service device\n");

	ret = devm_add_action_or_reset(dev, mtk_md_teardown, md);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, md);
	dev_info(dev, "starting the modem\n");
	schedule_work(&md->start_work);
	return 0;
}

static const struct of_device_id mtk_md_of_match[] = {
	{ .compatible = "mediatek,mt6771-md" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_md_of_match);

static struct platform_driver mtk_md_driver = {
	.probe = mtk_md_probe,
	.driver = {
		.name = "mtk_md",
		.of_match_table = mtk_md_of_match,
	},
};
module_platform_driver(mtk_md_driver);

MODULE_DESCRIPTION("MediaTek MT6771 integrated modem");
MODULE_LICENSE("GPL");

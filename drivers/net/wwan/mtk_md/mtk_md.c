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
 * Skeleton: the modem power-on is not implemented. It needs an MD1 power domain that mainline's
 * MT8183 SPM driver does not have, the vmodem rail and the modem PLL setup, so on hardware the
 * sequence stops at that step. Nothing here has run on a device yet.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/timekeeping.h>
#include <linux/workqueue.h>

#include "mtk_md_proto.h"
#include "mtk_md_regs.h"

/* BOOT_TIMEOUT in eccci/fsm/ccci_fsm_internal.h covers HS1 and HS2 together */
#define MTK_MD_HS_TIMEOUT_MS	30000

/* The CCCI MTU (3456) plus the CCCI header's allowance, less the header: AP_CCMNI_MTU */
#define MTK_MD_CCMNI_MTU	(CLDMA_MTU_SIZE - sizeof(struct mtk_md_ccci_hdr))

enum { MTK_MD_RST_CLDMA_AO, MTK_MD_RST_CLDMA_PD, MTK_MD_RST_CCIF, MTK_MD_RST_NUM };

static const char * const mtk_md_clk_names[] = {
	"cldma", "ccif-ap", "ccif-md", "ccif1-ap", "ccif1-md", "ccif2-ap", "ccif2-md",
};

struct mtk_md {
	struct device *dev;
	void __iomem *cldma_ao;
	void __iomem *cldma_pd;
	void __iomem *ap_ccif;
	void __iomem *md_ccif;
	int irq_cldma, irq_ccif0, irq_ccif1, irq_wdt;
	struct clk_bulk_data clks[ARRAY_SIZE(mtk_md_clk_names)];
	struct reset_control_bulk_data resets[MTK_MD_RST_NUM];
	bool clks_on;

	struct mtk_md_lk_hdr lk;
	struct mtk_md_lk_modem image;
	struct mtk_md_lk_smem smem;
	phys_addr_t smem_nc;		/* AP/MD1 non-cacheable share memory */
	u32 smem_nc_size;

	/* One idle descriptor per CLDMA queue: four TX, then one RX. */
	void *gpd;
	dma_addr_t gpd_dma;

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
	u32 v;
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

/* The data line (md_ccif_isr): ring queues 0-7 and the SRAM mailbox. */
static irqreturn_t mtk_md_ccif_data_irq(int irq, void *data)
{
	struct mtk_md *md = data;
	u32 ch = readl(md->ap_ccif + APCCIF_RCHNUM);

	writel(ch & CCIF_DATA_CHANNELS, md->ap_ccif + APCCIF_ACK);

	if (ch & BIT(CCIF_CH_SRAM)) {
		memcpy_fromio(md->hs1, md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_DL_HEADER,
			      sizeof(md->hs1));
		complete(&md->hs1_done);
	}
	if (ch & CCIF_DATA_CHANNELS & ~BIT(CCIF_CH_SRAM))
		dev_info_ratelimited(md->dev, "ccif: ring doorbell %#lx, rings not implemented\n",
				     ch & CCIF_DATA_CHANNELS & ~BIT(CCIF_CH_SRAM));

	return IRQ_HANDLED;
}

/* The control line (md_cd_ccif_isr): exception, wakeup and sequence-error channels. */
static irqreturn_t mtk_md_ccif_ctrl_irq(int irq, void *data)
{
	struct mtk_md *md = data;
	u32 ch = readl(md->ap_ccif + APCCIF_RCHNUM);

	writel(ch & CCIF_CTRL_CHANNELS, md->ap_ccif + APCCIF_ACK);

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

	dev_err(md->dev, "modem watchdog fired\n");
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
	memset_io(sram, 0, CCIF_SRAM_SIZE);
	memset_io(md->md_ccif + APCCIF_CHDATA, 0, CCIF_SRAM_SIZE);

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
	/* TODO: the vendor also sets infracfg 0xc00 bit 1 (CLDMA_IP_BUSY_MASK); no handle to it. */
}

/*
 * md_cd_power_on() and md_cd_let_md_go(). Not implemented: see the file header. What the vendor
 * does, for whoever writes it: topckgen CLK_MODE bits 8/9 clear, vmodem on, MD1 MTCMOS (SPM
 * 0x320) on, infracfg 0xf0c low byte 0x21, bus protection released, the MD PLL table at
 * 0x20140000/0x20150000, the MD watchdog off (0x200f0100 = 0x55000030), then the boot vector
 * enable (0x20000024 = 1).
 */
static int mtk_md_power_on(struct mtk_md *md)
{
	dev_err(md->dev, "step 4: modem power-on not implemented (MD1 domain, vmodem, MD PLL)\n");
	return -EOPNOTSUPP;
}

enum mtk_md_rt_kind {
	RT_SKIP,	/* the vendor has no case: nothing is appended */
	RT_EMPTY,	/* header only */
	RT_BOOT,
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
	[MTK_MD_RT_MISC_INFO_SBP_ID]		= { RT_MISC, true },
	[MTK_MD_RT_MISC_INFO_CCCI]		= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_CLIB_TIME]		= { RT_MISC },
	[MTK_MD_RT_MISC_INFO_C2K]		= { RT_MISC, true },
	[MTK_MD_RT_MD_IMAGE_START_MEMORY]	= { RT_SHM_ZERO, true },
	[MTK_MD_RT_CCISM_SHARE_MEMORY]		= { RT_SHM, false, 64 * SZ_1K, 32 * SZ_1K },
	[MTK_MD_RT_CCB_SHARE_MEMORY]		= { RT_MISC, true },
	[MTK_MD_RT_DHL_RAW_SHARE_MEMORY]	= { RT_SHM_ZERO, true },
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
		/* boot_channel CONTROL_RX; booting_start_id: normal boot, logging idle */
		w[0] = cpu_to_le32(MTK_MD_CH_CONTROL_RX);
		len = 16;
		break;
	case RT_SHM:
		w[0] = cpu_to_le32(mtk_md_smem_md_view(md->smem_nc + src->off));
		w[1] = cpu_to_le32(src->size);
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
			w[0] = cpu_to_le32(BIT(0) | BIT(1));	/* sequence check, MD status polling */
		} else if (id == MTK_MD_RT_MISC_INFO_CLIB_TIME) {
			ktime_get_real_ts64(&now);
			w[0] = cpu_to_le32(lower_32_bits(now.tv_sec));
			w[1] = cpu_to_le32(upper_32_bits(now.tv_sec));
		} else if (id == MTK_MD_RT_CCB_SHARE_MEMORY) {
			/* CCB control is non-cacheable; the data buffers are not mapped yet */
			w[0] = cpu_to_le32(mtk_md_smem_md_view(md->smem_nc + 96 * SZ_1K));
			w[1] = cpu_to_le32(4 * SZ_1K);
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
	phys_addr_t rt = md->smem_nc + MTK_MD_SMEM_RUNTIME_OFFSET;
	void __iomem *dst;
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

	dst = ioremap_wc(rt, MTK_MD_SMEM_RUNTIME_AP_SIZE);
	if (!dst) {
		ret = -ENOMEM;
		goto out;
	}
	memcpy_toio(dst, buf, pos);
	iounmap(dst);
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

	/* STAND-IN: no cacheable share memory (CCB data, DHL) is described to the modem yet */
	mtk_md_ap_query_fill(&aq, rt_md, mtk_md_smem_md_view(md->smem_nc), md->smem_nc_size, 0, 0);
	up.data[1] = cpu_to_le32(sizeof(up) + sizeof(aq));
	up.status = cpu_to_le32(FIELD_PREP(MTK_MD_CCCI_CHANNEL, MTK_MD_CH_CONTROL_TX));
	up.reserved = cpu_to_le32(MTK_MD_INIT_CHK_ID);
	sram = md->ap_ccif + APCCIF_CHDATA + CCIF_SRAM_UP_HEADER;
	memcpy_toio(sram, &up, sizeof(up));
	memcpy_toio(sram + sizeof(up), &aq, sizeof(aq));

	ret = mtk_md_ccif_send(md, CCIF_CH_SRAM);
	if (ret) {
		dev_err(md->dev, "step 12: CCIF SRAM channel busy\n");
		return ret;
	}
	/* HS2 (md_state 3 -> 4) arrives through CCIF ring queue 0, which is not implemented */
	dev_info(md->dev, "step 12: runtime data sent, HS2 not handled yet\n");
	return 0;
}

static void mtk_md_start(struct work_struct *work)
{
	struct mtk_md *md = container_of(work, struct mtk_md, start_work);
	int ret;

	dev_info(md->dev, "step 1: clocks on\n");
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(md->clks), md->clks);
	if (ret) {
		dev_err(md->dev, "step 1: clocks: %d\n", ret);
		return;
	}
	md->clks_on = true;

	dev_info(md->dev, "step 2: CCIF reset, SRAM cleared\n");
	mtk_md_ccif_reset(md);

	dev_info(md->dev, "step 3: CLDMA reset\n");
	mtk_md_cldma_hw_reset(md);

	ret = mtk_md_power_on(md);
	if (ret)
		return;

	dev_info(md->dev, "step 5: interrupts on\n");
	enable_irq(md->irq_wdt);
	enable_irq(md->irq_ccif1);
	enable_irq(md->irq_ccif0);
	enable_irq(md->irq_cldma);

	dev_info(md->dev, "step 6: cldma_reset (MTU %#x, SO_CFG enable)\n", CLDMA_MTU_SIZE);
	mtk_md_cldma_reset(md);

	dev_info(md->dev, "step 7: cldma_start (idle descriptors at %pad)\n", &md->gpd_dma);
	mtk_md_cldma_start(md);

	dev_info(md->dev, "step 8: started, modem booting\n");
	ret = mtk_md_handshake(md);
	if (ret)
		dev_err(md->dev, "handshake failed: %d\n", ret);
}

static void mtk_md_teardown(void *data)
{
	struct mtk_md *md = data;

	cancel_work_sync(&md->start_work);
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
	dev_info(dev, "mapped cldma-ao, cldma-pd, ap-ccif, md-ccif\n");

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

MODULE_DESCRIPTION("MediaTek MT6771 integrated modem (skeleton)");
MODULE_LICENSE("GPL");

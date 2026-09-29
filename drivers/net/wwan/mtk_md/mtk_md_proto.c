// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MD generation 6293 (MT6771) host protocol helpers.
 *
 * Pure functions over buffers, so they can be tested without the hardware. The formats are the
 * vendor's: ccci_util/ccci_util_lib_fo.c for the LK information block, eccci/fsm/ccci_fsm.c for
 * the control messages and eccci/ccci_modem.c for the runtime feature negotiation (MT6771 4.4
 * BSP). Where the vendor trusts the bootloader or the modem blindly, these check bounds instead.
 */

#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/math.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "mtk_md_proto.h"

#define LK_TAG_NAME_V1		16
#define LK_TAG_NAME_V2		64
#define LK_TAG_TAIL		12	/* data_offset, data_size, next_tag_offset */

#define LK_MODEM_LEN		24
#define LK_MODEM_MAX		4	/* the vendor reads at most four entries */
#define LK_SMEM_LEN		40

int mtk_md_lk_parse_hdr(const void *prop, size_t len, struct mtk_md_lk_hdr *hdr)
{
	const u8 *p = prop;
	int i;

	if (len < MTK_MD_LK_HDR_V2_LEN)
		return -EINVAL;

	hdr->base = get_unaligned_le64(p);
	hdr->size = get_unaligned_le32(p + 8);
	hdr->err_no = get_unaligned_le32(p + 12);
	hdr->version = get_unaligned_le32(p + 16);
	hdr->tag_num = get_unaligned_le32(p + 20);
	hdr->ld_flag = get_unaligned_le32(p + 24);
	for (i = 0; i < ARRAY_SIZE(hdr->ld_md_errno); i++)
		hdr->ld_md_errno[i] = get_unaligned_le32(p + 28 + 4 * i);

	if (!hdr->base)
		return hdr->err_no ? -EIO : -ENODEV;
	if (hdr->size > MTK_MD_LK_INFO_MAX)
		return -E2BIG;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_parse_hdr);

/*
 * Walk at most tag_num tags from the start of the list. next_tag_offset and data_offset both count
 * from the start of the list, and names are NUL-terminated and compared exactly.
 */
int mtk_md_lk_find_tag(const struct mtk_md_lk_info *info, const char *name, const void **data,
		       u32 *len)
{
	size_t name_len = info->version == 2 ? LK_TAG_NAME_V2 : LK_TAG_NAME_V1;
	size_t tag_len = name_len + LK_TAG_TAIL;
	size_t off = 0, end;
	u32 i, doff, dlen;

	for (i = 0; i < info->tag_num; i++) {
		const u8 *tag = info->buf + off;

		if (check_add_overflow(off, tag_len, &end) || end > info->size)
			return -EINVAL;
		if (!memchr(tag, '\0', name_len))
			return -EINVAL;

		if (!strcmp(tag, name)) {
			doff = get_unaligned_le32(tag + name_len);
			dlen = get_unaligned_le32(tag + name_len + 4);
			if (check_add_overflow((size_t)doff, (size_t)dlen, &end) ||
			    end > info->size)
				return -EINVAL;
			*data = info->buf + doff;
			*len = dlen;
			return 0;
		}

		off = get_unaligned_le32(tag + name_len + 8);
	}

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_find_tag);

/* As the vendor does, a tag whose size is not exactly the reader's is not that tag. */
static int lk_get_exact(const struct mtk_md_lk_info *info, const char *name, size_t want,
			const u8 **data)
{
	const void *p;
	u32 len;
	int ret;

	ret = mtk_md_lk_find_tag(info, name, &p, &len);
	if (ret)
		return ret;
	if (len != want)
		return -EINVAL;
	*data = p;
	return 0;
}

int mtk_md_lk_get_u32(const struct mtk_md_lk_info *info, const char *name, u32 *val)
{
	const u8 *p;
	int ret;

	ret = lk_get_exact(info, name, sizeof(u32), &p);
	if (ret)
		return ret;
	*val = get_unaligned_le32(p);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_get_u32);

int mtk_md_lk_get_modem(const struct mtk_md_lk_info *info, unsigned int idx,
			struct mtk_md_lk_modem *md)
{
	const void *data;
	const u8 *p;
	u32 count, len;
	int ret;

	ret = mtk_md_lk_get_u32(info, "hdr_count", &count);
	if (ret)
		return ret;
	if (idx >= count || idx >= LK_MODEM_MAX)
		return -ENOENT;

	ret = mtk_md_lk_find_tag(info, "hdr_tbl_inf", &data, &len);
	if (ret)
		return ret;
	if (len < (idx + 1) * LK_MODEM_LEN)
		return -EINVAL;

	p = (const u8 *)data + idx * LK_MODEM_LEN;
	md->base = get_unaligned_le64(p);
	md->size = get_unaligned_le32(p + 8);
	md->md_id = p[12];
	md->err_no = (s8)p[13];
	md->md_type = p[14];
	md->ver = p[15];
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_get_modem);

int mtk_md_lk_get_smem(const struct mtk_md_lk_info *info, struct mtk_md_lk_smem *smem)
{
	const u8 *p;
	int ret;

	ret = lk_get_exact(info, "smem_layout", LK_SMEM_LEN, &p);
	if (ret)
		return ret;

	smem->base = get_unaligned_le64(p);
	smem->ap_md1_offset = get_unaligned_le32(p + 8);
	smem->ap_md1_size = get_unaligned_le32(p + 12);
	smem->ap_md3_offset = get_unaligned_le32(p + 16);
	smem->ap_md3_size = get_unaligned_le32(p + 20);
	smem->md1_md3_offset = get_unaligned_le32(p + 24);
	smem->md1_md3_size = get_unaligned_le32(p + 28);
	smem->total_size = get_unaligned_le32(p + 32);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_get_smem);

/* ccci_fsm_recv_control_packet(): the message id is data[1], the check id is reserved. */
enum mtk_md_ctrl mtk_md_ctrl_classify(const struct mtk_md_ccci_hdr *hdr)
{
	switch (le32_to_cpu(hdr->data[1])) {
	case MTK_MD_CTRL_BOOT:
		if (le32_to_cpu(hdr->reserved) == MTK_MD_INIT_CHK_ID)
			return MTK_MD_CTRL_HS1;
		return MTK_MD_CTRL_HS2;
	case MTK_MD_CTRL_EX:
	case 0x5:	/* CCCI_DRV_VER_ERROR */
	case 0x6:	/* MD_EX_REC_OK */
	case 0x8:	/* MD_EX_PASS */
		return MTK_MD_CTRL_EXCEPTION;
	default:
		return MTK_MD_CTRL_UNKNOWN;
	}
}
EXPORT_SYMBOL_GPL(mtk_md_ctrl_classify);

/*
 * HS1 on generation 6293 is 240 bytes: the CCCI header, then the modem's feature query. A 16-byte
 * HS1 is the older handshake with no query, which this driver does not speak.
 */
int mtk_md_hs1_check(const void *buf, size_t len, const struct mtk_md_md_query **query)
{
	const struct mtk_md_ccci_hdr *hdr = buf;
	const struct mtk_md_md_query *q;

	if (len < MTK_MD_HS1_LEN)
		return -EPROTO;
	if (mtk_md_ctrl_classify(hdr) != MTK_MD_CTRL_HS1)
		return -EPROTO;

	q = (const void *)(hdr + 1);
	if (le32_to_cpu(q->head) != MTK_MD_FEATURE_QUERY_PATTERN ||
	    le32_to_cpu(q->tail) != MTK_MD_FEATURE_QUERY_PATTERN)
		return -EPROTO;

	*query = q;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_hs1_check);

/*
 * config_ap_side_feature() for MD_GENERATION >= 6293, all versions 0, assuming the build options
 * are off: no FEATURE_SCP_CCCI_SUPPORT, no ENABLE_32K_CLK_LESS, no ENABLE_FAST_HEADER, no tier-1
 * customer features, and no MD phy capture region.
 */
const u8 mtk_md_ap_features_6293[MTK_MD_FEATURE_COUNT] = {
	[MTK_MD_RT_BOOT_INFO]			= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_EXCEPTION_SHARE_MEMORY]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_CCIF_SHARE_MEMORY]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_SMART_LOGGING_SHARE_MEMORY]	= MTK_MD_FEATURE_NOT_SUPPORT,
	[MTK_MD_RT_MD1MD3_SHARE_MEMORY]		= MTK_MD_FEATURE_NOT_SUPPORT,
	[MTK_MD_RT_MISC_INFO_HIF_DMA_REMAP]	= MTK_MD_FEATURE_MUST,
	/* ENABLE_32K_CLK_LESS with no crystal: the modem derives its 32 kHz from the 26 MHz */
	[MTK_MD_RT_MISC_INFO_RTC_32K_LESS]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MISC_INFO_RANDOM_SEED_NUM]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MISC_INFO_GPS_COCLOCK]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MISC_INFO_SBP_ID]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MISC_INFO_CCCI]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MISC_INFO_CLIB_TIME]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MISC_INFO_C2K]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MD_IMAGE_START_MEMORY]	= MTK_MD_FEATURE_OPTIONAL,
	[MTK_MD_RT_CCISM_SHARE_MEMORY]		= MTK_MD_FEATURE_NOT_SUPPORT,
	[MTK_MD_RT_CCB_SHARE_MEMORY]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_DHL_RAW_SHARE_MEMORY]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_DT_NETD_SHARE_MEMORY]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_DT_USB_SHARE_MEMORY]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_EE_AFTER_EPOF]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_AP_CCMNI_MTU]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MISC_INFO_CUSTOMER_VAL]	= MTK_MD_FEATURE_NOT_SUPPORT,
	[MTK_MD_RT_MISC_INFO_C2K_MEID]		= MTK_MD_FEATURE_NOT_SUPPORT,
	[MTK_MD_RT_LWA_SHARE_MEMORY]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_AUDIO_RAW_SHARE_MEMORY]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MULTI_MD_MPU]		= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_CCISM_SHARE_MEMORY_EXP]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MD_PHY_CAPTURE]		= MTK_MD_FEATURE_NOT_SUPPORT,
	[MTK_MD_RT_MD_CONSYS_SHARE_MEMORY]	= MTK_MD_FEATURE_MUST,
	[MTK_MD_RT_MD_MTEE_SMEM_ENABLE]		= MTK_MD_FEATURE_OPTIONAL,
};
EXPORT_SYMBOL_GPL(mtk_md_ap_features_6293);

static u8 feature(unsigned int mask, unsigned int ver)
{
	return FIELD_PREP(MTK_MD_FEATURE_MASK, mask) | FIELD_PREP(MTK_MD_FEATURE_VER, ver);
}

/*
 * ccci_md_prepare_runtime_data(), the negotiation half. The modem's MUST and NOT_EXIST are echoed
 * as they came; OPTIONAL is accepted when the versions agree and the AP supports it;
 * BACKWARD_COMPAT when the modem's version is at least the AP's. NOT_SUPPORT has no case in the
 * vendor code, so its answer stays zero. A MUST the AP cannot meet ends the handshake.
 */
int mtk_md_rt_negotiate(const u8 *md_set, const u8 *ap_set, u8 *out, unsigned int *bad_id)
{
	unsigned int i;

	for (i = 0; i < MTK_MD_FEATURE_COUNT; i++) {
		unsigned int md_mask = FIELD_GET(MTK_MD_FEATURE_MASK, md_set[i]);
		unsigned int md_ver = FIELD_GET(MTK_MD_FEATURE_VER, md_set[i]);
		unsigned int ap_mask = FIELD_GET(MTK_MD_FEATURE_MASK, ap_set[i]);
		unsigned int ap_ver = FIELD_GET(MTK_MD_FEATURE_VER, ap_set[i]);
		bool ok;

		if (md_mask == MTK_MD_FEATURE_MUST && ap_mask < MTK_MD_FEATURE_MUST) {
			*bad_id = i;
			return -EPROTO;
		}

		switch (md_mask) {
		case MTK_MD_FEATURE_NOT_EXIST:
		case MTK_MD_FEATURE_MUST:
			out[i] = md_set[i];
			break;
		case MTK_MD_FEATURE_OPTIONAL:
		case MTK_MD_FEATURE_BACKWARD_COMPAT:
			if (md_mask == MTK_MD_FEATURE_OPTIONAL)
				ok = md_ver == ap_ver && ap_mask >= MTK_MD_FEATURE_MUST;
			else
				ok = md_ver >= ap_ver;
			out[i] = feature(ok ? MTK_MD_FEATURE_MUST : MTK_MD_FEATURE_NOT_SUPPORT,
					 ap_ver);
			break;
		default:
			out[i] = 0;
			break;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_rt_negotiate);

int mtk_md_rt_append(u8 *buf, size_t size, size_t *pos, u8 id, u8 support, const void *data,
		     u32 data_len)
{
	struct mtk_md_rt_feature f = {
		.feature_id = id,
		.support = support,
		.data_len = cpu_to_le32(data_len),
	};
	size_t end;

	if (check_add_overflow(*pos, sizeof(f) + (size_t)data_len, &end) || end > size)
		return -ENOSPC;

	memcpy(buf + *pos, &f, sizeof(f));
	if (data_len)
		memcpy(buf + *pos + sizeof(f), data, data_len);
	*pos = end;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_rt_append);

/*
 * md_cd_smem_sub_region_init() followed by pbm_v3 init_md1_section_level(), mt6771 values from
 * mach/mtk_pbm.h: per RAT, six 5-bit section levels packed from bit 0 upwards.
 */
static const struct {
	u8 word;
	u8 level[6];
} mtk_md_dbm_sections[] = {
	{ 20, { 31, 29, 27, 21, 15, 0 } },	/* SECTION_LEVLE_2G */
	{ 21, { 21, 20, 19, 16, 13, 0 } },	/* SECTION_LEVLE_3G */
	{ 22, { 20, 18, 16, 14, 11, 0 } },	/* SECTION_LEVLE_4G, uplink 1CC */
	{ 23, { 20, 18, 16, 14, 11, 0 } },	/* SECTION_1_LEVLE_4G, uplink 2CC */
	{ 35, { 21, 19, 18, 16, 13, 0 } },	/* SECTION_LEVLE_TDD */
	{ 36, { 23, 22, 20, 17, 16, 0 } },	/* SECTION_1_LEVLE_C2K */
};

#define MTK_MD_DBM_GUARD	0x44444444

void mtk_md_dbm_fill(__le32 *dbm)
{
	__le32 *body = dbm + 2;
	int i, s;

	memset(dbm, 0, MTK_MD_DBM_WORDS * 4);
	dbm[0] = cpu_to_le32(MTK_MD_DBM_GUARD);
	dbm[1] = cpu_to_le32(MTK_MD_DBM_GUARD);
	dbm[MTK_MD_DBM_WORDS - 2] = cpu_to_le32(MTK_MD_DBM_GUARD);
	dbm[MTK_MD_DBM_WORDS - 1] = cpu_to_le32(MTK_MD_DBM_GUARD);

	for (i = 0; i < ARRAY_SIZE(mtk_md_dbm_sections); i++) {
		u32 v = 0;

		for (s = 0; s < 6; s++)
			v |= mtk_md_dbm_sections[i].level[s] << (5 * s);
		body[mtk_md_dbm_sections[i].word] = cpu_to_le32(v);
	}
}
EXPORT_SYMBOL_GPL(mtk_md_dbm_fill);

/*
 * mt6771 ccb_configs[] (eccci/mt6771/ccci_platform.c): DL page, UL page, DL buffer, UL buffer.
 * Entries 0-17 are the DHL user (control, exception, PS, three hardware loggers, padding),
 * 18 the MD monitor, 19 META.
 */
static const u32 mtk_md_ccb_configs[][4] = {
	{ 1024, 1024, 32768, 32768 },
	{ 20480, 128, 122880, 128 },
	{ 512, 1024, 36864, 32768 },
	{ 512, 128, 32768, 128 },
	{ 512, 128, 32768, 128 },
	{ 512, 128, 131072, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 128, 128 }, { 128, 128, 128, 128 },
	{ 128, 128, 640, 128 },
	{ 512, 1024, 16384, 16384 },
	{ 66560, 66560, 532480, 532480 },
};

#define MTK_MD_CCB_CTRL_GUARD	0xeeff0011

/*
 * ccci_ccb_init_user() in the vendor's libccci_util.so, run by ccci_mdinit before the first
 * modem start: per config, a DL half and a UL half of eight words each, page size and buffer
 * size in words 5 and 6, a guard in word 7. The modem's CCB buffer manager asserts without it.
 */
void mtk_md_ccb_ctrl_fill(__le32 *ctrl)
{
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(mtk_md_ccb_configs) * 16 != MTK_MD_CCB_CTRL_WORDS);
	memset(ctrl, 0, MTK_MD_CCB_CTRL_WORDS * 4);
	for (i = 0; i < ARRAY_SIZE(mtk_md_ccb_configs); i++) {
		__le32 *e = ctrl + i * 16;

		e[5] = cpu_to_le32(mtk_md_ccb_configs[i][0]);
		e[6] = cpu_to_le32(mtk_md_ccb_configs[i][2]);
		e[7] = cpu_to_le32(MTK_MD_CCB_CTRL_GUARD);
		e[13] = cpu_to_le32(mtk_md_ccb_configs[i][1]);
		e[14] = cpu_to_le32(mtk_md_ccb_configs[i][3]);
		e[15] = cpu_to_le32(MTK_MD_CCB_CTRL_GUARD);
	}
}
EXPORT_SYMBOL_GPL(mtk_md_ccb_ctrl_fill);

/*
 * eccci/ccci_modem.c: "MD bank4 is remap to nearest 32M aligned address", so the modem's view of
 * a non-cacheable share memory address is its offset from the 32 MiB boundary below it, plus
 * 0x40000000.
 */
u32 mtk_md_smem_md_view(u64 ap_phys)
{
	return 0x40000000 + (u32)(ap_phys - round_down(ap_phys, SZ_32M));
}
EXPORT_SYMBOL_GPL(mtk_md_smem_md_view);

/* config_ap_runtime_data_v2_1() in eccci/modem_sys1.c. */
void mtk_md_ap_query_fill(struct mtk_md_ap_query *q, u32 ap_rt_addr, u32 noncached_start,
			  u32 noncached_size, u32 cached_start, u32 cached_size)
{
	memset(q, 0, sizeof(*q));
	q->head = cpu_to_le32(MTK_MD_AP_QUERY_PATTERN);
	/* version 1: the MPU size covers the AP/MD1 and MD1/MD3 shares; mask stays 0 */
	q->feature_set[1] = feature(0, 1);
	q->share_memory_support = cpu_to_le32(2);	/* MULTI_MD_MPU_SUPPORT */
	q->ap_rt_addr = cpu_to_le32(ap_rt_addr);
	q->ap_rt_size = cpu_to_le32(MTK_MD_SMEM_RUNTIME_AP_SIZE);
	q->md_rt_addr = cpu_to_le32(ap_rt_addr + MTK_MD_SMEM_RUNTIME_AP_SIZE);
	q->md_rt_size = cpu_to_le32(MTK_MD_SMEM_RUNTIME_MD_SIZE);
	q->noncached_mpu_start = cpu_to_le32(noncached_start);
	q->noncached_mpu_size = cpu_to_le32(noncached_size);
	q->cached_mpu_start = cpu_to_le32(cached_start);
	q->cached_mpu_size = cpu_to_le32(cached_size);
	q->tail = cpu_to_le32(MTK_MD_AP_QUERY_PATTERN);
}
EXPORT_SYMBOL_GPL(mtk_md_ap_query_fill);

const u32 mtk_md_ring_rx_size[MTK_MD_RING_QUEUES] = {
	80 * 1024, 80 * 1024, 40 * 1024, 80 * 1024, 20 * 1024, 20 * 1024, 64 * 1024, 0,
};
EXPORT_SYMBOL_GPL(mtk_md_ring_rx_size);

const u32 mtk_md_ring_tx_size[MTK_MD_RING_QUEUES] = {
	128 * 1024, 40 * 1024, 8 * 1024, 40 * 1024, 20 * 1024, 20 * 1024, 64 * 1024, 0,
};
EXPORT_SYMBOL_GPL(mtk_md_ring_tx_size);

const u32 mtk_md_ring_exp_size[MTK_MD_RING_QUEUES] = {
	12 * 1024, 32 * 1024, 8 * 1024, 0, 0, 0, 8 * 1024, 0,
};
EXPORT_SYMBOL_GPL(mtk_md_ring_exp_size);

/**
 * mtk_md_ring_create() - lay one queue out at the start of @buf
 * @used: the bytes the queue takes, for the caller to place the next one
 */
struct mtk_md_ring *mtk_md_ring_create(void *buf, size_t buf_size, u32 rx_size, u32 tx_size,
				       size_t *used)
{
	size_t len = MTK_MD_RING_CTL_LEN + rx_size + tx_size;
	struct mtk_md_ring *ring = buf + 8;
	__le32 *guard = buf;

	if (buf_size < len)
		return NULL;

	memset(buf, 0, len);
	guard[0] = cpu_to_le32(MTK_MD_RING_GUARD_HEAD);
	guard[1] = cpu_to_le32(MTK_MD_RING_GUARD_HEAD);
	guard = buf + len - 8;
	guard[0] = cpu_to_le32(MTK_MD_RING_GUARD_TAIL);
	guard[1] = cpu_to_le32(MTK_MD_RING_GUARD_TAIL);
	ring->rx_length = cpu_to_le32(rx_size);
	ring->tx_length = cpu_to_le32(tx_size);
	if (used)
		*used = len;
	return ring;
}
EXPORT_SYMBOL_GPL(mtk_md_ring_create);

static void mtk_md_ring_copy_out(const u8 *area, u32 length, u32 pos, void *out, u32 len)
{
	u32 first = min(len, length - pos);

	memcpy(out, area + pos, first);
	memcpy(out + first, area, len - first);
}

static void mtk_md_ring_copy_in(u8 *area, u32 length, u32 pos, const void *in, u32 len)
{
	u32 first = min(len, length - pos);

	memcpy(area + pos, in, first);
	memcpy(area, in + first, len - first);
}

/**
 * mtk_md_ring_rx_peek() - the length of the next message from the modem
 *
 * Return: the length, -ENODATA when the queue holds no whole message, -EBADMSG when what it
 * holds is not framed as a message. The pointers are the modem's to write, so they are checked.
 */
int mtk_md_ring_rx_peek(const struct mtk_md_ring *ring)
{
	u32 length = le32_to_cpu(ring->rx_length);
	u32 read = le32_to_cpu(ring->rx_read);
	u32 write = le32_to_cpu(ring->rx_write);
	__le32 w[2];
	u32 size, pkt;

	if (!length || read >= length || write >= length)
		return length ? -EBADMSG : -ENODATA;

	size = write >= read ? write - read : write + length - read;
	if (size < MTK_MD_RING_PKT_OVERHEAD + sizeof(struct mtk_md_ccci_hdr))
		return -ENODATA;

	mtk_md_ring_copy_out(ring->buffer, length, read, w, sizeof(w));
	if (le32_to_cpu(w[0]) != MTK_MD_RING_PKT_HEAD)
		return -EBADMSG;
	pkt = le32_to_cpu(w[1]);
	if (pkt > length)
		return -EBADMSG;
	if (ALIGN(pkt + MTK_MD_RING_PKT_OVERHEAD, 8) > size)
		return -ENODATA;

	mtk_md_ring_copy_out(ring->buffer, length,
			     (read + ALIGN(pkt + MTK_MD_RING_PKT_OVERHEAD, 8) - 8) % length,
			     w, sizeof(w));
	if (le32_to_cpu(w[0]) != MTK_MD_RING_PKT_TAIL || le32_to_cpu(w[1]) != MTK_MD_RING_PKT_TAIL)
		return -EBADMSG;

	return pkt;
}
EXPORT_SYMBOL_GPL(mtk_md_ring_rx_peek);

/* Copy out the message mtk_md_ring_rx_peek() found; @len is what it returned. */
void mtk_md_ring_rx_read(const struct mtk_md_ring *ring, void *out, u32 len)
{
	u32 length = le32_to_cpu(ring->rx_length);

	mtk_md_ring_copy_out(ring->buffer, length, (le32_to_cpu(ring->rx_read) + 8) % length,
			     out, len);
}
EXPORT_SYMBOL_GPL(mtk_md_ring_rx_read);

void mtk_md_ring_rx_consume(struct mtk_md_ring *ring, u32 len)
{
	u32 length = le32_to_cpu(ring->rx_length);
	u32 read = le32_to_cpu(ring->rx_read);

	read = ALIGN(read + len + MTK_MD_RING_PKT_OVERHEAD, 8);
	ring->rx_read = cpu_to_le32(read >= length ? read - length : read);
}
EXPORT_SYMBOL_GPL(mtk_md_ring_rx_consume);

/**
 * mtk_md_ring_tx_write() - queue one message for the modem
 *
 * Return: 0, or -ENOSPC when the queue cannot take it now. One byte always stays free.
 */
int mtk_md_ring_tx_write(struct mtk_md_ring *ring, const void *data, u32 len)
{
	u32 length = le32_to_cpu(ring->tx_length);
	u32 read = le32_to_cpu(ring->tx_read);
	u32 write = le32_to_cpu(ring->tx_write);
	u8 *area = ring->buffer + le32_to_cpu(ring->rx_length);
	__le32 head[2] = { cpu_to_le32(MTK_MD_RING_PKT_HEAD), cpu_to_le32(len) };
	__le32 tail[2] = { cpu_to_le32(MTK_MD_RING_PKT_TAIL), cpu_to_le32(MTK_MD_RING_PKT_TAIL) };
	u32 need = ALIGN(len + MTK_MD_RING_PKT_OVERHEAD, 8);
	u32 room;

	if (!len || !length || read >= length || write >= length)
		return -EINVAL;

	room = read > write ? read - write - 1 : length - write - 1 + read;
	if (need >= room)
		return -ENOSPC;

	mtk_md_ring_copy_in(area, length, write, head, sizeof(head));
	mtk_md_ring_copy_in(area, length, (write + 8) % length, data, len);
	mtk_md_ring_copy_in(area, length, (write + need - 8) % length, tail, sizeof(tail));
	/* the message before the pointer that announces it */
	mb();
	ring->tx_write = cpu_to_le32((write + need) % length);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_ring_tx_write);

/**
 * mtk_md_rpc_parse() - split an RPC request into its parameters
 * @msg: the whole message, CCCI header first
 *
 * The parameters point into @msg. Everything the modem wrote is checked against @len.
 */
int mtk_md_rpc_parse(const void *msg, size_t len, struct mtk_md_rpc_req *req)
{
	const u8 *p = msg, *end = p + len;
	u32 i, n;

	if (len < sizeof(struct mtk_md_ccci_hdr) + 8 || len > MTK_MD_RPC_MAX_LEN)
		return -EINVAL;
	p += sizeof(struct mtk_md_ccci_hdr);
	req->op = get_unaligned_le32(p);
	req->argc = get_unaligned_le32(p + 4);
	p += 8;
	if (req->argc > MTK_MD_RPC_MAX_ARGS)
		return -EINVAL;

	for (i = 0; i < req->argc; i++) {
		if (end - p < 4)
			return -EINVAL;
		n = get_unaligned_le32(p);
		p += 4;
		if (n > end - p)
			return -EINVAL;
		req->arg[i] = p;
		req->arg_len[i] = n;
		p += min_t(size_t, ALIGN(n, 4), end - p);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_rpc_parse);

/**
 * mtk_md_rpc_build() - the answer to an RPC request
 * @req_hdr: the request's CCCI header; its first word and its buffer index go back unchanged
 *
 * Return: the length of the message in @buf, or -ENOSPC.
 */
int mtk_md_rpc_build(void *buf, size_t size, const struct mtk_md_ccci_hdr *req_hdr, u32 op,
		     u32 argc, const void *const *arg, const u32 *arg_len)
{
	struct mtk_md_ccci_hdr *h = buf;
	size_t pos = sizeof(*h) + 8;
	u8 *p = buf;
	u32 i;

	if (argc > MTK_MD_RPC_MAX_ARGS || size < pos)
		return -ENOSPC;
	for (i = 0; i < argc; i++) {
		if (pos + 4 + ALIGN(arg_len[i], 4) > min_t(size_t, size, MTK_MD_RPC_MAX_LEN))
			return -ENOSPC;
		put_unaligned_le32(arg_len[i], p + pos);
		memset(p + pos + 4, 0, ALIGN(arg_len[i], 4));
		memcpy(p + pos + 4, arg[i], arg_len[i]);
		pos += 4 + ALIGN(arg_len[i], 4);
	}

	h->data[0] = req_hdr->data[0];
	h->data[1] = cpu_to_le32(pos);
	h->status = cpu_to_le32(FIELD_PREP(MTK_MD_CCCI_CHANNEL, MTK_MD_CH_RPC_TX));
	h->reserved = req_hdr->reserved;
	put_unaligned_le32(op | MTK_MD_RPC_RESP, p + sizeof(*h));
	put_unaligned_le32(argc, p + sizeof(*h) + 4);
	return pos;
}
EXPORT_SYMBOL_GPL(mtk_md_rpc_build);

/**
 * mtk_md_ipc_parse() - take an IPC message from the modem apart
 * @msg: the whole message, CCCI header first
 *
 * The destination is the header's reserved word, as port_ipc_kernel_thread() takes it. The
 * local_para block points into @msg and its msg_len is checked against @len, which the vendor
 * does not do.
 */
int mtk_md_ipc_parse(const void *msg, size_t len, struct mtk_md_ipc_msg *ipc)
{
	const size_t off = sizeof(struct mtk_md_ccci_hdr) + MTK_MD_IPC_ILM_LEN;
	const struct mtk_md_ccci_hdr *h = msg;
	const u8 *p = msg;
	u16 para_len;

	if (len < off + MTK_MD_IPC_PARA_HDR)
		return -EINVAL;
	para_len = get_unaligned_le16(p + off + 2);
	if (para_len < MTK_MD_IPC_PARA_HDR || para_len > len - off)
		return -EINVAL;

	p += sizeof(*h);
	ipc->src = get_unaligned_le32(p);
	ipc->dest = le32_to_cpu(h->reserved);
	ipc->sap = get_unaligned_le32(p + 8);
	ipc->msg_id = get_unaligned_le32(p + 12);
	ipc->para = p + MTK_MD_IPC_ILM_LEN;
	ipc->para_len = para_len;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_md_ipc_parse);

/* ccci_ipc_task_ID.h: the modem modules of the id table (MD_MOD_USBCLASS 11 is not in it) */
static bool mtk_md_ipc_md_module(u32 id)
{
	return id <= 12 && id != 11;
}

/* the AP tasks that have an IPC port (AP_IPC_AGPS 0 .. AP_IPC_LWAPROXY 9) */
static bool mtk_md_ipc_ap_task(u32 id)
{
	return (id & MTK_MD_IPC_AP) && (id & ~MTK_MD_IPC_AP) <= 9;
}

/**
 * mtk_md_ipc_build() - an IPC message for the modem, as port_ipc_kernel_write() makes it
 *
 * The sequence number is left to the sender.
 *
 * Return: the length of the message in @buf, -EINVAL for ids the vendor refuses or a local_para
 * shorter than its header, -EMSGSIZE beyond CCCI_MTU, or -ENOSPC.
 */
int mtk_md_ipc_build(void *buf, size_t size, const struct mtk_md_ipc_msg *ipc)
{
	struct mtk_md_ccci_hdr *h = buf;
	size_t len = sizeof(*h) + MTK_MD_IPC_ILM_LEN + ipc->para_len;
	u8 *p = buf;

	if (!mtk_md_ipc_ap_task(ipc->src) || !mtk_md_ipc_md_module(ipc->dest) ||
	    ipc->para_len < MTK_MD_IPC_PARA_HDR)
		return -EINVAL;
	if (MTK_MD_IPC_ILM_LEN + ipc->para_len > MTK_MD_CCCI_MTU)
		return -EMSGSIZE;
	if (size < len)
		return -ENOSPC;

	h->data[0] = 0;
	h->data[1] = cpu_to_le32(len);
	h->status = cpu_to_le32(FIELD_PREP(MTK_MD_CCCI_CHANNEL, MTK_MD_CH_IPC_TX));
	h->reserved = cpu_to_le32(ipc->dest);
	p += sizeof(*h);
	put_unaligned_le32(ipc->src, p);
	put_unaligned_le32(ipc->dest, p + 4);
	put_unaligned_le32(ipc->sap, p + 8);
	put_unaligned_le32(ipc->msg_id, p + 12);
	/* local_para_ptr: anything but NULL for the modem */
	put_unaligned_le32(1, p + 16);
	put_unaligned_le32(0, p + 20);
	memcpy(p + MTK_MD_IPC_ILM_LEN, ipc->para, ipc->para_len);
	return len;
}
EXPORT_SYMBOL_GPL(mtk_md_ipc_build);

MODULE_DESCRIPTION("MediaTek MT6771 modem protocol helpers");
MODULE_LICENSE("GPL");

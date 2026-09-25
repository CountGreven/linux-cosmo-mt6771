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
	[MTK_MD_RT_MISC_INFO_RTC_32K_LESS]	= MTK_MD_FEATURE_NOT_SUPPORT,
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

MODULE_DESCRIPTION("MediaTek MT6771 modem protocol helpers");
MODULE_LICENSE("GPL");

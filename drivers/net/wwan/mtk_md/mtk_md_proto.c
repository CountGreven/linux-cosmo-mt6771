// SPDX-License-Identifier: GPL-2.0-only
/* Stubs: the tests come first. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>

#include "mtk_md_proto.h"

int mtk_md_lk_parse_hdr(const void *prop, size_t len, struct mtk_md_lk_hdr *hdr)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_parse_hdr);

int mtk_md_lk_find_tag(const struct mtk_md_lk_info *info, const char *name, const void **data,
		       u32 *len)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_find_tag);

int mtk_md_lk_get_u32(const struct mtk_md_lk_info *info, const char *name, u32 *val)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_get_u32);

int mtk_md_lk_get_modem(const struct mtk_md_lk_info *info, unsigned int idx,
			struct mtk_md_lk_modem *md)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_get_modem);

int mtk_md_lk_get_smem(const struct mtk_md_lk_info *info, struct mtk_md_lk_smem *smem)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_lk_get_smem);

enum mtk_md_ctrl mtk_md_ctrl_classify(const struct mtk_md_ccci_hdr *hdr)
{
	return MTK_MD_CTRL_UNKNOWN;
}
EXPORT_SYMBOL_GPL(mtk_md_ctrl_classify);

int mtk_md_hs1_check(const void *buf, size_t len, const struct mtk_md_md_query **query)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_hs1_check);

const u8 mtk_md_ap_features_6293[MTK_MD_FEATURE_COUNT];
EXPORT_SYMBOL_GPL(mtk_md_ap_features_6293);

int mtk_md_rt_negotiate(const u8 *md_set, const u8 *ap_set, u8 *out, unsigned int *bad_id)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_rt_negotiate);

int mtk_md_rt_append(u8 *buf, size_t size, size_t *pos, u8 id, u8 support, const void *data,
		     u32 data_len)
{
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(mtk_md_rt_append);

MODULE_DESCRIPTION("MediaTek MT6771 modem protocol helpers");
MODULE_LICENSE("GPL");

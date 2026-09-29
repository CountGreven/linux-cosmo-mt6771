// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the MT6771 modem protocol helpers.
 *
 * The LK header is captured: the bytes of /chosen "ccci,modem_info_v2" read off a running Cosmo
 * (dtc -I fs, 2026-09-24). The tag list it points at cannot be captured the same way -- the
 * vendor kernel wipes it with memset_io() right after parsing -- so it is synthesised here from
 * the vendor's format, with the values the vendor kernel logged for this device where it did.
 */

#include <kunit/test.h>
#include <linux/unaligned.h>
#include <linux/string.h>

#include "mtk_md_proto.h"

/* dtc prints the property as big-endian cells; these are its raw bytes. */
static const u8 cosmo_modem_info_v2[48] = {
	0x00, 0x00, 0x00, 0x8c, 0x00, 0x00, 0x00, 0x00,	/* base */
	0xc8, 0x0b, 0x00, 0x00,				/* size */
	0x00, 0x00, 0x00, 0x00,				/* err_no */
	0x02, 0x00, 0x00, 0x00,				/* version */
	0x1a, 0x00, 0x00, 0x00,				/* tag_num */
	0x01, 0x00, 0x00, 0x00,				/* ld_flag */
};

static void lk_hdr_captured(struct kunit *test)
{
	struct mtk_md_lk_hdr hdr;

	KUNIT_ASSERT_EQ(test, mtk_md_lk_parse_hdr(cosmo_modem_info_v2, sizeof(cosmo_modem_info_v2),
						  &hdr), 0);
	KUNIT_EXPECT_EQ(test, hdr.base, 0x8c000000ULL);
	KUNIT_EXPECT_EQ(test, hdr.size, 0xbc8U);
	KUNIT_EXPECT_EQ(test, hdr.err_no, 0);
	KUNIT_EXPECT_EQ(test, hdr.version, 2);
	KUNIT_EXPECT_EQ(test, hdr.tag_num, 26U);
	KUNIT_EXPECT_EQ(test, hdr.ld_flag, 1U);
	KUNIT_EXPECT_EQ(test, hdr.ld_md_errno[0], 0);
}

static void lk_hdr_fields_only(struct kunit *test)
{
	const u8 *p = cosmo_modem_info_v2;
	struct mtk_md_lk_hdr hdr;

	/* LK may omit the tail padding; the fields are all there is to read. */
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(p, MTK_MD_LK_HDR_V2_LEN, &hdr), 0);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(p, MTK_MD_LK_HDR_V2_LEN - 1, &hdr), -EINVAL);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(p, 16, &hdr), -EINVAL);
}

static void lk_hdr_rejects(struct kunit *test)
{
	struct mtk_md_lk_hdr hdr;
	u8 p[48];

	/* No base and no error: LK loaded nothing, which the vendor treats as "no modem". */
	memset(p, 0, sizeof(p));
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(p, sizeof(p), &hdr), -ENODEV);

	/* No base with an error: LK tried and failed. */
	p[12] = 0xfe;
	p[13] = 0xff;
	p[14] = 0xff;
	p[15] = 0xff;
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(p, sizeof(p), &hdr), -EIO);
	KUNIT_EXPECT_EQ(test, hdr.err_no, -2);

	/* A tag list larger than the vendor ever maps. */
	memcpy(p, cosmo_modem_info_v2, sizeof(p));
	p[8] = 0x01;
	p[9] = 0x00;
	p[10] = 0x01;
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(p, sizeof(p), &hdr), -E2BIG);
}

/* Synthesising a tag list in the vendor's layout: tags first, each pointing at data further on. */
struct blob {
	u8 buf[1024];
	size_t name_len;	/* 64 for version 2, 16 otherwise */
	size_t tags_end;
	size_t data_end;
	u32 last_tag;
	u32 tag_num;
};

static void blob_init(struct blob *b, int version, u32 tag_capacity)
{
	memset(b, 0, sizeof(*b));
	b->name_len = version == 2 ? 64 : 16;
	b->tags_end = 0;
	b->data_end = tag_capacity * (b->name_len + 12);
}

static void put_le32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static void put_le64(u8 *p, u64 v)
{
	put_le32(p, v);
	put_le32(p + 4, v >> 32);
}

static void blob_add(struct blob *b, const char *name, const void *data, u32 len)
{
	u8 *tag = b->buf + b->tags_end;

	strscpy(tag, name, b->name_len);
	put_le32(tag + b->name_len, b->data_end);
	put_le32(tag + b->name_len + 4, len);
	/* next_tag_offset counts from the start of the list, not from this tag */
	put_le32(tag + b->name_len + 8, b->tags_end + b->name_len + 12);
	memcpy(b->buf + b->data_end, data, len);
	b->last_tag = b->tags_end;
	b->tags_end += b->name_len + 12;
	b->data_end += len;
	b->tag_num++;
}

static struct mtk_md_lk_info blob_info(struct blob *b, int version)
{
	return (struct mtk_md_lk_info) {
		.buf = b->buf, .size = b->data_end, .tag_num = b->tag_num, .version = version,
	};
}

/*
 * The Cosmo's list as far as the vendor log shows it: MD1 at 0x66000000, 0x7e00000 bytes
 * ("ccci_md0 at LK"), share memory at 0x8c000000, 1 MiB ("ccci_share_mem at LK"), MD type 12
 * (CCCI_IOC_GET_MD_TYPE). The AP/MD1 share starts at offset 0: the log remaps CCB control, 96 KiB
 * into it, at 0x8c018000. Its size and md1_phy_cap are not logged; those values are made up.
 */
static void cosmo_blob(struct blob *b)
{
	u8 modem[24] = { 0 }, smem[40] = { 0 }, word[4];

	blob_init(b, 2, 8);
	put_le32(word, 1);
	blob_add(b, "hdr_count", word, 4);

	put_le64(modem, 0x66000000);
	put_le32(modem + 8, 0x7e00000);
	modem[12] = 0;		/* md_id: MD1 */
	modem[13] = 0;		/* errno */
	modem[14] = 12;		/* md_type */
	modem[15] = 1;		/* ver (made up) */
	blob_add(b, "hdr_tbl_inf", modem, sizeof(modem));

	put_le32(word, 0x02800000);	/* made up */
	blob_add(b, "md1_phy_cap", word, 4);

	put_le64(smem, 0x8c000000);
	put_le32(smem + 8, 0x0);	/* ap_md1 offset */
	put_le32(smem + 12, 0x80000);	/* ap_md1 size, made up */
	put_le32(smem + 32, 0x100000);	/* total */
	blob_add(b, "smem_layout", smem, sizeof(smem));
}

static void lk_tags_cosmo(struct kunit *test)
{
	struct mtk_md_lk_modem md;
	struct mtk_md_lk_smem smem;
	struct mtk_md_lk_info info;
	struct blob *b;
	u32 v;

	b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	cosmo_blob(b);
	info = blob_info(b, 2);

	KUNIT_ASSERT_EQ(test, mtk_md_lk_get_u32(&info, "hdr_count", &v), 0);
	KUNIT_EXPECT_EQ(test, v, 1U);

	KUNIT_ASSERT_EQ(test, mtk_md_lk_get_modem(&info, 0, &md), 0);
	KUNIT_EXPECT_EQ(test, md.base, 0x66000000ULL);
	KUNIT_EXPECT_EQ(test, md.size, 0x7e00000U);
	KUNIT_EXPECT_EQ(test, md.md_id, 0);
	KUNIT_EXPECT_EQ(test, md.err_no, 0);
	KUNIT_EXPECT_EQ(test, md.md_type, 12);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_get_modem(&info, 1, &md), -ENOENT);

	KUNIT_ASSERT_EQ(test, mtk_md_lk_get_smem(&info, &smem), 0);
	KUNIT_EXPECT_EQ(test, smem.base, 0x8c000000ULL);
	KUNIT_EXPECT_EQ(test, smem.ap_md1_size, 0x80000U);
	KUNIT_EXPECT_EQ(test, smem.total_size, 0x100000U);

	/* found by exact name, and only by exact name */
	KUNIT_EXPECT_EQ(test, mtk_md_lk_get_u32(&info, "md1_phy_cap", &v), 0);
	KUNIT_EXPECT_EQ(test, v, 0x02800000U);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_get_u32(&info, "md1_phy", &v), -ENOENT);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_get_u32(&info, "no_such_tag", &v), -ENOENT);
}

static void lk_tags_v1(struct kunit *test)
{
	struct mtk_md_lk_info info;
	struct blob *b;
	u8 word[4];
	u32 v;

	b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	blob_init(b, 1, 2);
	put_le32(word, 7);
	blob_add(b, "opt_md1_support", word, 4);
	put_le32(word, 12);
	blob_add(b, "hdr_count", word, 4);
	info = blob_info(b, 1);

	KUNIT_ASSERT_EQ(test, mtk_md_lk_get_u32(&info, "hdr_count", &v), 0);
	KUNIT_EXPECT_EQ(test, v, 12U);
	KUNIT_ASSERT_EQ(test, mtk_md_lk_get_u32(&info, "opt_md1_support", &v), 0);
	KUNIT_EXPECT_EQ(test, v, 7U);
}

static void lk_tags_malformed(struct kunit *test)
{
	struct mtk_md_lk_info info;
	struct blob *b;
	const void *data;
	u32 len, v;
	u8 *tag;

	b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);

	/* A size that does not match what the reader expects is an error, not a truncation. */
	cosmo_blob(b);
	info = blob_info(b, 2);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_get_u32(&info, "smem_layout", &v), -EINVAL);

	/* The walk stops at tag_num, even if the list claims more. */
	info.tag_num = 1;
	KUNIT_EXPECT_EQ(test, mtk_md_lk_find_tag(&info, "smem_layout", &data, &len), -ENOENT);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_find_tag(&info, "hdr_count", &data, &len), 0);

	/* Data running off the end of the list. */
	cosmo_blob(b);
	tag = b->buf + b->last_tag;
	put_le32(tag + 64 + 4, 0x1000);
	info = blob_info(b, 2);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_find_tag(&info, "smem_layout", &data, &len), -EINVAL);

	/* ... or wrapping around when offset and size are added. */
	put_le32(tag + 64, 0xfffffff0);
	put_le32(tag + 64 + 4, 0x20);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_find_tag(&info, "smem_layout", &data, &len), -EINVAL);

	/* A next pointer that leaves the list. */
	cosmo_blob(b);
	put_le32(b->buf + 64 + 8, 0x10000);
	info = blob_info(b, 2);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_find_tag(&info, "smem_layout", &data, &len), -EINVAL);

	/* A name that fills its field with no terminator. */
	cosmo_blob(b);
	memset(b->buf, 'x', 64);
	info = blob_info(b, 2);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_find_tag(&info, "smem_layout", &data, &len), -EINVAL);

	/* A list too short to hold even one tag. */
	cosmo_blob(b);
	info = blob_info(b, 2);
	info.size = 40;
	KUNIT_EXPECT_EQ(test, mtk_md_lk_find_tag(&info, "hdr_count", &data, &len), -EINVAL);
}

static void ctrl_classify(struct kunit *test)
{
	struct mtk_md_ccci_hdr h = { };

	/* What the Cosmo logged as "control message 0x0,0x5555FFFF", then "0x0,0x0". */
	h.data[1] = cpu_to_le32(MTK_MD_CTRL_BOOT);
	h.reserved = cpu_to_le32(MTK_MD_INIT_CHK_ID);
	KUNIT_EXPECT_EQ(test, mtk_md_ctrl_classify(&h), MTK_MD_CTRL_HS1);

	h.reserved = 0;
	KUNIT_EXPECT_EQ(test, mtk_md_ctrl_classify(&h), MTK_MD_CTRL_HS2);

	h.data[1] = cpu_to_le32(MTK_MD_CTRL_EX);
	h.reserved = cpu_to_le32(MTK_MD_EX_CHK_ID);
	KUNIT_EXPECT_EQ(test, mtk_md_ctrl_classify(&h), MTK_MD_CTRL_EXCEPTION);

	h.data[1] = cpu_to_le32(0x7);
	KUNIT_EXPECT_EQ(test, mtk_md_ctrl_classify(&h), MTK_MD_CTRL_UNKNOWN);
}

static void user_hdr(struct kunit *test)
{
	struct mtk_md_ccci_hdr h;

	/* speech mailbox 0x2F21 as the HAL writes it (SPH_OFF), then an SPH_ON payload */
	h.data[0] = cpu_to_le32(0xffffffff);
	h.data[1] = cpu_to_le32(0x2f210000);
	h.status = cpu_to_le32(0x5);
	h.reserved = cpu_to_le32(0x1234);
	mtk_md_user_hdr_tx(&h, 16, 5);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.data[0]), 0xffffffff);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.data[1]), 0x2f210000);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.status), 5);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.reserved), 0x1234);

	h.data[0] = 0;
	h.data[1] = cpu_to_le32(128 + 6);
	h.status = cpu_to_le32(0xabcd0005);
	h.reserved = cpu_to_le32(0x2f200086);
	mtk_md_user_hdr_tx(&h, 128 + 22, 5);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.data[0]), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.data[1]), 150);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.status), 5);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(h.reserved), 0x2f200086);

	h.data[0] = cpu_to_le32(0xffffffff);
	KUNIT_EXPECT_EQ(test, mtk_md_user_hdr_rx_len(&h, 16), 16);
	KUNIT_EXPECT_EQ(test, mtk_md_user_hdr_rx_len(&h, 64), 16);
	h.data[0] = 0;
	KUNIT_EXPECT_EQ(test, mtk_md_user_hdr_rx_len(&h, 64), 64);
}

static void hs1_check(struct kunit *test)
{
	const struct mtk_md_md_query *q;
	struct mtk_md_ccci_hdr *h;
	struct mtk_md_md_query *mq;
	u8 *buf;

	KUNIT_EXPECT_EQ(test, MTK_MD_HS1_LEN, 240);	/* the vendor's HS1 length */

	buf = kunit_kzalloc(test, MTK_MD_HS1_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	h = (struct mtk_md_ccci_hdr *)buf;
	mq = (struct mtk_md_md_query *)(buf + sizeof(*h));
	h->reserved = cpu_to_le32(MTK_MD_INIT_CHK_ID);
	mq->head = cpu_to_le32(MTK_MD_FEATURE_QUERY_PATTERN);
	mq->tail = cpu_to_le32(MTK_MD_FEATURE_QUERY_PATTERN);
	mq->feature_set[MTK_MD_RT_BOOT_INFO] = MTK_MD_FEATURE_MUST;

	KUNIT_ASSERT_EQ(test, mtk_md_hs1_check(buf, MTK_MD_HS1_LEN, &q), 0);
	KUNIT_EXPECT_PTR_EQ(test, (const void *)q, (const void *)mq);

	/* An old-style HS1, header only, carries no query. */
	KUNIT_EXPECT_EQ(test, mtk_md_hs1_check(buf, sizeof(*h), &q), -EPROTO);

	mq->tail = cpu_to_le32(MTK_MD_AP_QUERY_PATTERN);
	KUNIT_EXPECT_EQ(test, mtk_md_hs1_check(buf, MTK_MD_HS1_LEN, &q), -EPROTO);

	mq->tail = cpu_to_le32(MTK_MD_FEATURE_QUERY_PATTERN);
	h->reserved = 0;
	KUNIT_EXPECT_EQ(test, mtk_md_hs1_check(buf, MTK_MD_HS1_LEN, &q), -EPROTO);
}

#define FEAT(mask, ver)	((mask) | ((ver) << 4))

static void rt_negotiate(struct kunit *test)
{
	u8 md[MTK_MD_FEATURE_COUNT] = { }, ap[MTK_MD_FEATURE_COUNT] = { };
	u8 out[MTK_MD_FEATURE_COUNT];
	unsigned int bad;

	md[0] = FEAT(MTK_MD_FEATURE_MUST, 0);
	ap[0] = FEAT(MTK_MD_FEATURE_MUST, 0);
	md[1] = FEAT(MTK_MD_FEATURE_OPTIONAL, 1);
	ap[1] = FEAT(MTK_MD_FEATURE_MUST, 1);
	md[2] = FEAT(MTK_MD_FEATURE_OPTIONAL, 2);
	ap[2] = FEAT(MTK_MD_FEATURE_MUST, 1);
	md[3] = FEAT(MTK_MD_FEATURE_BACKWARD_COMPAT, 3);
	ap[3] = FEAT(MTK_MD_FEATURE_MUST, 2);
	md[4] = FEAT(MTK_MD_FEATURE_BACKWARD_COMPAT, 1);
	ap[4] = FEAT(MTK_MD_FEATURE_MUST, 2);
	md[5] = FEAT(MTK_MD_FEATURE_NOT_SUPPORT, 3);
	ap[5] = FEAT(MTK_MD_FEATURE_MUST, 0);
	md[6] = FEAT(MTK_MD_FEATURE_OPTIONAL, 0);
	ap[6] = FEAT(MTK_MD_FEATURE_NOT_SUPPORT, 0);

	KUNIT_ASSERT_EQ(test, mtk_md_rt_negotiate(md, ap, out, &bad), 0);
	KUNIT_EXPECT_EQ(test, out[0], FEAT(MTK_MD_FEATURE_MUST, 0));	/* MD's own byte */
	KUNIT_EXPECT_EQ(test, out[1], FEAT(MTK_MD_FEATURE_MUST, 1));	/* versions agree */
	KUNIT_EXPECT_EQ(test, out[2], FEAT(MTK_MD_FEATURE_NOT_SUPPORT, 1));
	KUNIT_EXPECT_EQ(test, out[3], FEAT(MTK_MD_FEATURE_MUST, 2));	/* MD is newer */
	KUNIT_EXPECT_EQ(test, out[4], FEAT(MTK_MD_FEATURE_NOT_SUPPORT, 2));
	/* The vendor has no case for NOT_SUPPORT and leaves the byte zeroed. */
	KUNIT_EXPECT_EQ(test, out[5], 0);
	KUNIT_EXPECT_EQ(test, out[6], FEAT(MTK_MD_FEATURE_NOT_SUPPORT, 0));
	KUNIT_EXPECT_EQ(test, out[7], 0);

	/* The MD insisting on something the AP lacks ends the handshake. */
	md[40] = FEAT(MTK_MD_FEATURE_MUST, 0);
	KUNIT_EXPECT_EQ(test, mtk_md_rt_negotiate(md, ap, out, &bad), -EPROTO);
	KUNIT_EXPECT_EQ(test, bad, 40U);
}

static void rt_ap_features_6293(struct kunit *test)
{
	const u8 *ap = mtk_md_ap_features_6293;

	/* A few from config_ap_side_feature(), the 6293 branch. */
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_BOOT_INFO], MTK_MD_FEATURE_MUST);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_CCIF_SHARE_MEMORY], MTK_MD_FEATURE_MUST);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_CCISM_SHARE_MEMORY], MTK_MD_FEATURE_NOT_SUPPORT);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MD1MD3_SHARE_MEMORY], MTK_MD_FEATURE_NOT_SUPPORT);
	/* the Cosmo has no 32 kHz crystal: the vendor kernel answers MUST (ENABLE_32K_CLK_LESS) */
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MISC_INFO_RTC_32K_LESS], MTK_MD_FEATURE_MUST);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MD_IMAGE_START_MEMORY], MTK_MD_FEATURE_OPTIONAL);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MULTI_MD_MPU], MTK_MD_FEATURE_MUST);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MD_MTEE_SMEM_ENABLE], MTK_MD_FEATURE_OPTIONAL);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_CCCI_FAST_HEADER], MTK_MD_FEATURE_NOT_EXIST);
	KUNIT_EXPECT_EQ(test, ap[23], MTK_MD_FEATURE_NOT_EXIST);
}

static void rt_append(struct kunit *test)
{
	u8 buf[24];
	__le32 boot[2] = { cpu_to_le32(0), cpu_to_le32(0x12345678) };
	size_t pos = 0;

	KUNIT_ASSERT_EQ(test, mtk_md_rt_append(buf, sizeof(buf), &pos, 0, 2, boot, 8), 0);
	KUNIT_EXPECT_EQ(test, pos, 16);
	KUNIT_EXPECT_EQ(test, buf[0], 0);
	KUNIT_EXPECT_EQ(test, buf[1], 2);
	KUNIT_EXPECT_EQ(test, buf[4], 8);
	KUNIT_EXPECT_EQ(test, buf[12], 0x78);

	/* header only, as the vendor sends for features it does not accept */
	KUNIT_ASSERT_EQ(test, mtk_md_rt_append(buf, sizeof(buf), &pos, 3, 1, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, pos, 24);
	KUNIT_EXPECT_EQ(test, buf[16], 3);

	KUNIT_EXPECT_EQ(test, mtk_md_rt_append(buf, sizeof(buf), &pos, 4, 1, NULL, 0), -ENOSPC);
	KUNIT_EXPECT_EQ(test, pos, 24);
}

static void smem_md_view(struct kunit *test)
{
	/* The modem sees its non-cacheable bank 4 at 0x40000000, from the 32 MiB boundary down. */
	KUNIT_EXPECT_EQ(test, mtk_md_smem_md_view(0x8c000000), 0x40000000U);
	KUNIT_EXPECT_EQ(test, mtk_md_smem_md_view(0x8c000000 + 58 * 1024), 0x4000e800U);
	KUNIT_EXPECT_EQ(test, mtk_md_smem_md_view(0x8b000000), 0x41000000U);
}

static void ap_query(struct kunit *test)
{
	struct mtk_md_ap_query q;
	const u8 *b = (const u8 *)&q;

	KUNIT_ASSERT_EQ(test, sizeof(q), 156);
	KUNIT_ASSERT_EQ(test, sizeof(struct mtk_md_ccci_hdr) + sizeof(q), 172);	/* vendor packet */

	mtk_md_ap_query_fill(&q, 0x4000e800, 0x40000000, 0x100000, 0, 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.head), MTK_MD_AP_QUERY_PATTERN);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.tail), MTK_MD_AP_QUERY_PATTERN);
	/* feature_set[1] carries only a version: "the MPU size includes the MD1/MD3 share" */
	KUNIT_EXPECT_EQ(test, q.feature_set[0], 0);
	KUNIT_EXPECT_EQ(test, q.feature_set[1], 0x10);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.share_memory_support), 2U);	/* MULTI_MD_MPU_SUPPORT */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.ap_rt_addr), 0x4000e800U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.ap_rt_size), 0x800U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.md_rt_addr), 0x4000f000U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.md_rt_size), 0x800U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.noncached_mpu_start), 0x40000000U);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(q.noncached_mpu_size), 0x100000U);
	/* byte offsets, as the modem reads them */
	KUNIT_EXPECT_EQ(test, b[68], 2);
	KUNIT_EXPECT_EQ(test, b[72], 0x00);
	KUNIT_EXPECT_EQ(test, b[73], 0xe8);
	KUNIT_EXPECT_EQ(test, b[152], 0x49);
}

/* The queue the modem would see: the same block with the directions swapped. */
static struct mtk_md_ring *ring_peer(struct kunit *test, const struct mtk_md_ring *ring)
{
	u32 rx = le32_to_cpu(ring->rx_length), tx = le32_to_cpu(ring->tx_length);
	struct mtk_md_ring *peer;

	peer = kunit_kzalloc(test, sizeof(*peer) + rx + tx, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	peer->rx_read = ring->tx_read;
	peer->rx_write = ring->tx_write;
	peer->rx_length = ring->tx_length;
	peer->tx_read = ring->rx_read;
	peer->tx_write = ring->rx_write;
	peer->tx_length = ring->rx_length;
	memcpy(peer->buffer, ring->buffer + rx, tx);
	memcpy(peer->buffer + tx, ring->buffer, rx);
	return peer;
}

static void ring_layout(struct kunit *test)
{
	size_t total = 0, used;
	u8 *buf;
	int q;

	/* the vendor's tables fill the two regions of the 6293 share memory */
	for (q = 0; q < MTK_MD_RING_QUEUES; q++)
		total += MTK_MD_RING_CTL_LEN + mtk_md_ring_rx_size[q] + mtk_md_ring_tx_size[q];
	KUNIT_EXPECT_LE(test, total, (size_t)705 * 1024);
	total = 0;
	for (q = 0; q < MTK_MD_RING_QUEUES; q++)
		total += MTK_MD_RING_CTL_LEN + 2 * mtk_md_ring_exp_size[q];
	KUNIT_EXPECT_LE(test, total, (size_t)121 * 1024);

	buf = kunit_kzalloc(test, 256, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_EXPECT_NULL(test, mtk_md_ring_create(buf, 40 + 64 + 32 - 1, 64, 32, &used));
	KUNIT_ASSERT_NOT_NULL(test, mtk_md_ring_create(buf, 256, 64, 32, &used));
	KUNIT_EXPECT_EQ(test, used, (size_t)40 + 64 + 32);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf), 0xee0000eeU);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 4), 0xee0000eeU);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 16), 64U);	/* rx length */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 28), 32U);	/* tx length */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + used - 8), 0xff0000ffU);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + used - 4), 0xff0000ffU);
}

static void ring_framing(struct kunit *test)
{
	static const u8 msg[21] = "0123456789abcdefghij";
	struct mtk_md_ring *ring;
	u8 *buf, *tx;

	buf = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	ring = mtk_md_ring_create(buf, 512, 128, 128, NULL);
	KUNIT_ASSERT_NOT_NULL(test, ring);
	tx = ring->buffer + 128;

	KUNIT_EXPECT_EQ(test, mtk_md_ring_rx_peek(ring), -ENODATA);
	KUNIT_ASSERT_EQ(test, mtk_md_ring_tx_write(ring, msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(tx), 0xaabbaabbU);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(tx + 4), 21U);
	KUNIT_EXPECT_MEMEQ(test, tx + 8, msg, sizeof(msg));
	/* 21 bytes are padded to 24; then the two tail markers */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(tx + 32), 0xccddeeffU);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(tx + 36), 0xccddeeffU);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ring->tx_write), 40U);
}

static void ring_roundtrip_wraps(struct kunit *test)
{
	struct mtk_md_ring *ring, *peer;
	u8 msg[40], out[40];
	int i, round;
	u8 *buf;

	buf = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	ring = mtk_md_ring_create(buf, 512, 96, 96, NULL);
	KUNIT_ASSERT_NOT_NULL(test, ring);

	/* 56 bytes a message in a 96-byte area: every second one wraps */
	for (round = 0; round < 7; round++) {
		for (i = 0; i < sizeof(msg); i++)
			msg[i] = round * 16 + i;
		KUNIT_ASSERT_EQ(test, mtk_md_ring_tx_write(ring, msg, sizeof(msg)), 0);

		peer = ring_peer(test, ring);
		KUNIT_ASSERT_EQ(test, mtk_md_ring_rx_peek(peer), (int)sizeof(msg));
		mtk_md_ring_rx_read(peer, out, sizeof(out));
		KUNIT_EXPECT_MEMEQ(test, out, msg, sizeof(msg));
		mtk_md_ring_rx_consume(peer, sizeof(msg));
		KUNIT_EXPECT_EQ(test, mtk_md_ring_rx_peek(peer), -ENODATA);
		ring->tx_read = peer->rx_read;
	}
}

static void ring_full_and_bad(struct kunit *test)
{
	struct mtk_md_ring *ring;
	u8 msg[40] = { 1 };
	u8 *buf;

	buf = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	ring = mtk_md_ring_create(buf, 512, 96, 96, NULL);
	KUNIT_ASSERT_NOT_NULL(test, ring);

	KUNIT_EXPECT_EQ(test, mtk_md_ring_tx_write(ring, msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, mtk_md_ring_tx_write(ring, msg, sizeof(msg)), -ENOSPC);
	KUNIT_EXPECT_EQ(test, mtk_md_ring_tx_write(ring, msg, 0), -EINVAL);

	/* what the modem wrote is not trusted: pointers and framing are checked */
	ring->rx_write = cpu_to_le32(40);
	KUNIT_EXPECT_EQ(test, mtk_md_ring_rx_peek(ring), -EBADMSG);
	put_le32(ring->buffer, 0xaabbaabb);
	put_le32(ring->buffer + 4, 24);
	KUNIT_EXPECT_EQ(test, mtk_md_ring_rx_peek(ring), -EBADMSG);	/* no tail */
	put_le32(ring->buffer + 32, 0xccddeeff);
	put_le32(ring->buffer + 36, 0xccddeeff);
	KUNIT_EXPECT_EQ(test, mtk_md_ring_rx_peek(ring), 24);
	put_le32(ring->buffer + 4, 4000);
	KUNIT_EXPECT_EQ(test, mtk_md_ring_rx_peek(ring), -EBADMSG);
	ring->rx_write = cpu_to_le32(96);
	KUNIT_EXPECT_EQ(test, mtk_md_ring_rx_peek(ring), -EBADMSG);
}

/* The modem's first request on the Cosmo, as it arrived: SIM1's hot-plug source pin */
static const u8 rpc_eint_req[] = {
	0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00,		/* data[0], length 68 */
	0x20, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,		/* channel 32, assert, index 0 */
	0x05, 0x40, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,		/* 0x4005, three parameters */
	0x17, 0x00, 0x00, 0x00, 'M', 'D', '1', '_', 'S', 'I', 'M', '1', '_', 'H', 'O', 'T',
	'_', 'P', 'L', 'U', 'G', '_', 'E', 'I', 'N', 'T', 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,		/* type 6: source pin */
};

static void rpc_parse(struct kunit *test)
{
	struct mtk_md_rpc_req req;
	u8 bad[sizeof(rpc_eint_req)];

	KUNIT_ASSERT_EQ(test, sizeof(rpc_eint_req), (size_t)68);
	KUNIT_ASSERT_EQ(test, mtk_md_rpc_parse(rpc_eint_req, sizeof(rpc_eint_req), &req), 0);
	KUNIT_EXPECT_EQ(test, req.op, 0x4005U);
	KUNIT_EXPECT_EQ(test, req.argc, 3U);
	KUNIT_EXPECT_EQ(test, req.arg_len[0], 23U);
	KUNIT_EXPECT_STREQ(test, (const char *)req.arg[0], "MD1_SIM1_HOT_PLUG_EINT");
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(req.arg[2]), 6U);

	/* a length that runs past the message, and too many parameters */
	memcpy(bad, rpc_eint_req, sizeof(bad));
	put_le32(bad + 24, 200);
	KUNIT_EXPECT_EQ(test, mtk_md_rpc_parse(bad, sizeof(bad), &req), -EINVAL);
	memcpy(bad, rpc_eint_req, sizeof(bad));
	put_le32(bad + 20, 7);
	KUNIT_EXPECT_EQ(test, mtk_md_rpc_parse(bad, sizeof(bad), &req), -EINVAL);
	KUNIT_EXPECT_EQ(test, mtk_md_rpc_parse(bad, 20, &req), -EINVAL);
}

static void rpc_build(struct kunit *test)
{
	const struct mtk_md_ccci_hdr *req = (const void *)rpc_eint_req;
	u32 ok = 0, pin = 1, three = 0x0a0b0c;
	const void *arg[] = { &ok, &pin, &three };
	u32 len[] = { 4, 4, 3 };
	u8 buf[64];
	int n;

	n = mtk_md_rpc_build(buf, sizeof(buf), req, 0x4005, 3, arg, len);
	KUNIT_ASSERT_EQ(test, n, 16 + 8 + 8 + 8 + 8);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 4), (u32)n);		/* data[1] */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 8) & 0xffff, 33U);	/* RPC_TX */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 12), 0U);		/* index back */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 16), 0xffff4005U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 20), 3U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 24), 4U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 36), 1U);		/* the pin */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 40), 3U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 44), 0x0a0b0cU);		/* padded with 0 */

	KUNIT_EXPECT_EQ(test, mtk_md_rpc_build(buf, 40, req, 0x4005, 3, arg, len), -ENOSPC);
}

/*
 * EL1 -> WMT LTE_DEFAULT_PARAM_IND as port_ipc_kernel_thread() takes it apart: the destination is
 * the header's reserved word, not the ILM's own dest field.
 */
static const u8 ipc_el1_to_wmt[48] = {
	0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,	/* data[0], data[1] = 48 */
	0x22, 0x00, 0x07, 0x00,				/* ch 34, seq 7 */
	0x03, 0x00, 0x00, 0x80,				/* AP_MOD_WMT */
	0x05, 0x00, 0x00, 0x00,				/* src MD_MOD_EL1 */
	0x03, 0x00, 0x00, 0x00,				/* ILM dest, ignored */
	0x00, 0x00, 0x00, 0x00,				/* sap */
	0x46, 0x00, 0x00, 0x80,				/* msg id */
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* local_para_ptr, peer_buff_ptr */
	0x00, 0x00, 0x08, 0x00,				/* ref_count, _stub, msg_len 8 */
	0x11, 0x22, 0x33, 0x44,
};

static void ipc_parse(struct kunit *test)
{
	struct mtk_md_ipc_msg ipc;
	u8 bad[52];

	KUNIT_ASSERT_EQ(test, mtk_md_ipc_parse(ipc_el1_to_wmt, sizeof(ipc_el1_to_wmt), &ipc), 0);
	KUNIT_EXPECT_EQ(test, ipc.src, 5U);
	KUNIT_EXPECT_EQ(test, ipc.dest, 0x80000003U);
	KUNIT_EXPECT_EQ(test, ipc.sap, 0U);
	KUNIT_EXPECT_EQ(test, ipc.msg_id, 0x80000046U);
	KUNIT_EXPECT_PTR_EQ(test, ipc.para, &ipc_el1_to_wmt[40]);
	KUNIT_EXPECT_EQ(test, ipc.para_len, 8);

	/* bytes behind the block are allowed */
	memcpy(bad, ipc_el1_to_wmt, sizeof(ipc_el1_to_wmt));
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_parse(bad, sizeof(bad), &ipc), 0);
	KUNIT_EXPECT_EQ(test, ipc.para_len, 8);

	KUNIT_EXPECT_EQ(test, mtk_md_ipc_parse(bad, 43, &ipc), -EINVAL);
	bad[42] = 9;		/* one more than the message holds */
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_parse(bad, 48, &ipc), -EINVAL);
	bad[42] = 3;		/* shorter than its own header */
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_parse(bad, 48, &ipc), -EINVAL);
}

/* port_ipc_kernel_write(): WMT -> EL1 WIFIBT_OPER_DEFAULT_PARAM_IND */
static void ipc_build(struct kunit *test)
{
	static const u8 para[6] = { 0x00, 0x00, 0x06, 0x00, 0xaa, 0xbb };
	struct mtk_md_ipc_msg ipc = {
		.src = 0x80000003, .dest = 5, .sap = 0, .msg_id = 0x80000042,
		.para = para, .para_len = sizeof(para),
	}, back;
	u8 buf[64];
	int n;

	memset(buf, 0xa5, sizeof(buf));
	n = mtk_md_ipc_build(buf, sizeof(buf), &ipc);
	KUNIT_ASSERT_EQ(test, n, 16 + 24 + 6);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 4), (u32)n);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 8), 36U);		/* IPC_TX, seq 0 */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 12), 5U);		/* unify id */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 16), 0x80000003U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 20), 5U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 24), 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 28), 0x80000042U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 32), 1U);		/* "not NULL" */
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(buf + 36), 0U);
	KUNIT_EXPECT_MEMEQ(test, buf + 40, para, sizeof(para));

	KUNIT_ASSERT_EQ(test, mtk_md_ipc_parse(buf, n, &back), 0);
	KUNIT_EXPECT_EQ(test, back.src, ipc.src);
	KUNIT_EXPECT_EQ(test, back.dest, ipc.dest);
	KUNIT_EXPECT_EQ(test, back.msg_id, ipc.msg_id);
	KUNIT_EXPECT_EQ(test, back.para_len, ipc.para_len);

	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, 45, &ipc), -ENOSPC);
}

static void ipc_build_rejects(struct kunit *test)
{
	static u8 para[MTK_MD_CCCI_MTU];
	struct mtk_md_ipc_msg ipc = {
		.src = 0x80000003, .dest = 5, .msg_id = 0x80000042, .para = para, .para_len = 4,
	};
	u8 buf[64];

	/* the destination must be a modem module of ccci_ipc_task_ID.h */
	ipc.dest = 11;		/* MD_MOD_USBCLASS is not in the table */
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, sizeof(buf), &ipc), -EINVAL);
	ipc.dest = 0x80000003;
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, sizeof(buf), &ipc), -EINVAL);
	ipc.dest = 12;		/* MD_MOD_WAAL */
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, sizeof(buf), &ipc), 44);

	/* the source must be an AP task that has a port */
	ipc.src = 3;
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, sizeof(buf), &ipc), -EINVAL);
	ipc.src = 0x8000000a;
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, sizeof(buf), &ipc), -EINVAL);
	ipc.src = 0x80000003;

	ipc.para_len = 3;
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, sizeof(buf), &ipc), -EINVAL);
	/* ILM and block within CCCI_MTU */
	ipc.para_len = MTK_MD_CCCI_MTU - 24 + 1;
	KUNIT_EXPECT_EQ(test, mtk_md_ipc_build(buf, sizeof(buf), &ipc), -EMSGSIZE);
}

/* md_cd_smem_sub_region_init() and pbm_v3 init_md1_section_level() for mt6771, by hand */
static void dbm_fill(struct kunit *test)
{
	__le32 dbm[MTK_MD_DBM_WORDS];
	int i;

	memset(dbm, 0xa5, sizeof(dbm));
	mtk_md_dbm_fill(dbm);

	KUNIT_EXPECT_EQ(test, MTK_MD_DBM_WORDS * 4, 176);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[0]), 0x44444444);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[1]), 0x44444444);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[42]), 0x44444444);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[43]), 0x44444444);
	/* body word n is dbm[2 + n]: section levels 2G, 3G, 4G, 4G_1, TDD, C2K_1 */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[2 + 20]), 0xfaefbf);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[2 + 21]), 0xd84e95);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[2 + 22]), 0xb74254);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[2 + 23]), 0xb74254);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[2 + 35]), 0xd84a75);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(dbm[2 + 36]), 0x108d2d7);
	for (i = 2; i < 42; i++) {
		if (i - 2 == 20 || i - 2 == 21 || i - 2 == 22 || i - 2 == 23 ||
		    i - 2 == 35 || i - 2 == 36)
			continue;
		KUNIT_EXPECT_EQ_MSG(test, le32_to_cpu(dbm[i]), 0, "word %d", i);
	}
}

/* ccci_ccb_init_user() in libccci_util.so, fed with mt6771 ccb_configs[] */
static void ccb_ctrl_fill(struct kunit *test)
{
	__le32 ctrl[MTK_MD_CCB_CTRL_WORDS];
	int e, w;

	memset(ctrl, 0xa5, sizeof(ctrl));
	mtk_md_ccb_ctrl_fill(ctrl);

	KUNIT_EXPECT_EQ(test, MTK_MD_CCB_CTRL_WORDS * 4, 20 * 64);
	for (e = 0; e < 20; e++) {
		for (w = 0; w < 5; w++) {
			KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[e * 16 + w]), 0);
			KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[e * 16 + 8 + w]), 0);
		}
		KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[e * 16 + 7]), 0xeeff0011);
		KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[e * 16 + 15]), 0xeeff0011);
	}
	/* DHL control pages: 1 KiB pages, 32 of them each way */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[5]), 1024);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[6]), 32768);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[13]), 1024);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[14]), 32768);
	/* DHL exception: 20 KiB x 6 down, one 128-byte dummy page up */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[16 + 5]), 20480);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[16 + 6]), 122880);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[16 + 13]), 128);
	/* the padding entry, then MD monitor and META */
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[17 * 16 + 6]), 640);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[18 * 16 + 5]), 512);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[18 * 16 + 14]), 16384);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[19 * 16 + 6]), 532480);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(ctrl[19 * 16 + 13]), 66560);
}

static struct kunit_case mtk_md_proto_cases[] = {
	KUNIT_CASE(rpc_parse),
	KUNIT_CASE(rpc_build),
	KUNIT_CASE(ipc_parse),
	KUNIT_CASE(ipc_build),
	KUNIT_CASE(ipc_build_rejects),
	KUNIT_CASE(ring_layout),
	KUNIT_CASE(ring_framing),
	KUNIT_CASE(ring_roundtrip_wraps),
	KUNIT_CASE(ring_full_and_bad),
	KUNIT_CASE(smem_md_view),
	KUNIT_CASE(ap_query),
	KUNIT_CASE(lk_hdr_captured),
	KUNIT_CASE(lk_hdr_fields_only),
	KUNIT_CASE(lk_hdr_rejects),
	KUNIT_CASE(lk_tags_cosmo),
	KUNIT_CASE(lk_tags_v1),
	KUNIT_CASE(lk_tags_malformed),
	KUNIT_CASE(ctrl_classify),
	KUNIT_CASE(user_hdr),
	KUNIT_CASE(hs1_check),
	KUNIT_CASE(rt_negotiate),
	KUNIT_CASE(rt_ap_features_6293),
	KUNIT_CASE(rt_append),
	KUNIT_CASE(dbm_fill),
	KUNIT_CASE(ccb_ctrl_fill),
	{}
};

static struct kunit_suite mtk_md_proto_suite = {
	.name = "mtk_md_proto",
	.test_cases = mtk_md_proto_cases,
};
kunit_test_suite(mtk_md_proto_suite);

MODULE_DESCRIPTION("KUnit tests for the MT6771 modem protocol helpers");
MODULE_LICENSE("GPL");

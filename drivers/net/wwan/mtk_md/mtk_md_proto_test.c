// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the MT6771 modem protocol helpers.
 *
 * The LK header is captured: the bytes of /chosen "ccci,modem_info_v2" read off a running Cosmo
 * (dtc -I fs, 2026-09-24). The tag list it points at cannot be captured the same way -- the vendor
 * kernel wipes it with memset_io() right after parsing -- so it is synthesised here from the vendor's
 * format, with the values the vendor kernel logged for this device where it logged any.
 */

#include <kunit/test.h>
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
	struct mtk_md_lk_hdr hdr;

	/* LK may omit the tail padding; the fields are all there is to read. */
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(cosmo_modem_info_v2, MTK_MD_LK_HDR_V2_LEN, &hdr), 0);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(cosmo_modem_info_v2, MTK_MD_LK_HDR_V2_LEN - 1, &hdr),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, mtk_md_lk_parse_hdr(cosmo_modem_info_v2, 16, &hdr), -EINVAL);
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
 * The Cosmo's list as far as the vendor log shows it: MD1 at 0x66000000, 0x7e00000 bytes ("ccci_md0 at
 * LK"), share memory at 0x8c000000, 1 MiB ("ccci_share_mem at LK"), MD type 12 (CCCI_IOC_GET_MD_TYPE).
 * The AP/MD1 share starts at offset 0: the log remaps CCB control, 96 KiB into it, at 0x8c018000. Its
 * size and md1_phy_cap are not logged; those values are made up.
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
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MD_IMAGE_START_MEMORY], MTK_MD_FEATURE_OPTIONAL);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MULTI_MD_MPU], MTK_MD_FEATURE_MUST);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_MD_MTEE_SMEM_ENABLE], MTK_MD_FEATURE_OPTIONAL);
	KUNIT_EXPECT_EQ(test, ap[MTK_MD_RT_CCCI_FAST_HEADER], MTK_MD_FEATURE_NOT_EXIST);
	KUNIT_EXPECT_EQ(test, ap[23], MTK_MD_FEATURE_NOT_EXIST);
}

static void rt_append(struct kunit *test)
{
	u8 buf[24];
	u32 boot[2] = { cpu_to_le32(0), cpu_to_le32(0x12345678) };
	size_t pos = 0;

	KUNIT_ASSERT_EQ(test, mtk_md_rt_append(buf, sizeof(buf), &pos, 0, 2, boot, sizeof(boot)), 0);
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
	/* The modem sees its non-cacheable bank 4 at 0x40000000, remapped from the 32 MiB boundary. */
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

static struct kunit_case mtk_md_proto_cases[] = {
	KUNIT_CASE(smem_md_view),
	KUNIT_CASE(ap_query),
	KUNIT_CASE(lk_hdr_captured),
	KUNIT_CASE(lk_hdr_fields_only),
	KUNIT_CASE(lk_hdr_rejects),
	KUNIT_CASE(lk_tags_cosmo),
	KUNIT_CASE(lk_tags_v1),
	KUNIT_CASE(lk_tags_malformed),
	KUNIT_CASE(ctrl_classify),
	KUNIT_CASE(hs1_check),
	KUNIT_CASE(rt_negotiate),
	KUNIT_CASE(rt_ap_features_6293),
	KUNIT_CASE(rt_append),
	{}
};

static struct kunit_suite mtk_md_proto_suite = {
	.name = "mtk_md_proto",
	.test_cases = mtk_md_proto_cases,
};
kunit_test_suite(mtk_md_proto_suite);

MODULE_DESCRIPTION("KUnit tests for the MT6771 modem protocol helpers");
MODULE_LICENSE("GPL");

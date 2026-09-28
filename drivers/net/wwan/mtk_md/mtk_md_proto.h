/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek MD generation 6293 (MT6771) host protocol: the pieces that are pure data and can be
 * tested without hardware -- the LK information block, the CCCI header, the control-message
 * classification and the runtime feature negotiation.
 *
 * Every layout here is the vendor's (drivers/misc/mediatek/eccci and ccci_util in the MT6771 4.4
 * BSP), little-endian, and was cross-checked against the device where the device shows it.
 */

#ifndef __MTK_MD_PROTO_H__
#define __MTK_MD_PROTO_H__

#include <linux/bits.h>
#include <linux/types.h>

/*
 * LK describes what it loaded in /chosen "ccci,modem_info_v2": a C struct copied into the
 * property as raw CPU bytes, so little-endian, not the usual big-endian cells. The live Cosmo tree
 * carries 48 bytes: base 0x8c000000, size 0xbc8, version 2, 26 tags.
 */
#define MTK_MD_LK_HDR_V2_LEN		44	/* fields; sizeof() is 48 with tail padding */
#define MTK_MD_LK_INFO_MAX		0x10000	/* the vendor maps and accepts no more */

struct mtk_md_lk_hdr {
	u64 base;		/* physical address of the tag list */
	u32 size;		/* bytes of tag list */
	s32 err_no;
	s32 version;		/* 2: 64-byte tag names; otherwise 16 */
	u32 tag_num;
	u32 ld_flag;
	s32 ld_md_errno[4];
};

int mtk_md_lk_parse_hdr(const void *prop, size_t len, struct mtk_md_lk_hdr *hdr);

/* One view of the tag list, however it was mapped. */
struct mtk_md_lk_info {
	const u8 *buf;
	size_t size;
	u32 tag_num;
	int version;
};

int mtk_md_lk_find_tag(const struct mtk_md_lk_info *info, const char *name, const void **data,
		       u32 *len);
int mtk_md_lk_get_u32(const struct mtk_md_lk_info *info, const char *name, u32 *val);

/* "hdr_tbl_inf": one entry per loaded modem image, "hdr_count" of them. */
struct mtk_md_lk_modem {
	u64 base;
	u32 size;
	u8 md_id;		/* 0 is MD1 */
	s8 err_no;		/* 0: LK loaded it */
	u8 md_type;
	u8 ver;
};

int mtk_md_lk_get_modem(const struct mtk_md_lk_info *info, unsigned int idx,
			struct mtk_md_lk_modem *md);

/* "smem_layout": the AP/MD share memory. */
struct mtk_md_lk_smem {
	u64 base;
	u32 ap_md1_offset;
	u32 ap_md1_size;
	u32 ap_md3_offset;
	u32 ap_md3_size;
	u32 md1_md3_offset;
	u32 md1_md3_size;
	u32 total_size;
};

int mtk_md_lk_get_smem(const struct mtk_md_lk_info *info, struct mtk_md_lk_smem *smem);

/*
 * The CCCI header that leads every message. The third word packs channel (15:0), sequence
 * (30:16) and an assert bit (31); the last word carries a check id on control messages.
 */
struct mtk_md_ccci_hdr {
	__le32 data[2];
	__le32 status;
	__le32 reserved;
} __packed;

#define MTK_MD_CCCI_CHANNEL		GENMASK(15, 0)
#define MTK_MD_CCCI_SEQ			GENMASK(30, 16)
#define MTK_MD_CCCI_ASSERT		BIT(31)

#define MTK_MD_CH_CONTROL_RX		0
#define MTK_MD_CH_CONTROL_TX		1
#define MTK_MD_CH_SYSTEM_RX		2	/* port_sysmsg.c: ids in data[1], value in reserved */
#define MTK_MD_CH_SYSTEM_TX		3

#define MTK_MD_SYS_TX_POWER_SWTP	0x10e	/* AP: SAR mode, 0 without a swtp node */
#define MTK_MD_SYS_BATTERY_INFO		0x105	/* both ways: battery voltage in mV */
#define MTK_MD_SYS_SIM_TYPE		0x107
#define MTK_MD_SYS_SWTP_REQ		0x110
#define MTK_MD_SYS_TEST_MD2AP		0x114	/* echoed as 0x115 */
#define MTK_MD_SYS_TEST_L1CORE		0x116	/* echoed as 0x117 */

#define MTK_MD_CTRL_BOOT		0x0		/* data[1] of both handshakes */
#define MTK_MD_CTRL_EX			0x4
#define MTK_MD_INIT_CHK_ID		0x5555ffff	/* reserved of HS1 and the runtime data */
#define MTK_MD_EX_CHK_ID		0x45584350	/* "EXCP" */

enum mtk_md_ctrl {
	MTK_MD_CTRL_HS1,	/* MD booted, sends its feature query: md_state 2 -> 3 */
	MTK_MD_CTRL_HS2,	/* MD accepted our runtime data: md_state 3 -> 4, ready */
	MTK_MD_CTRL_EXCEPTION,
	MTK_MD_CTRL_UNKNOWN,
};

enum mtk_md_ctrl mtk_md_ctrl_classify(const struct mtk_md_ccci_hdr *hdr);

/*
 * Runtime features. Each side states, per feature id, a support level in bits 3:0 and a version
 * in bits 7:4 of one byte. The MD sends its 64 in HS1; the AP answers with a negotiated level per
 * feature and a TLV list of what each accepted feature needs (mostly share memory addresses).
 */
#define MTK_MD_FEATURE_COUNT		64
#define MTK_MD_FEATURE_QUERY_PATTERN	0x49434343	/* MD -> AP, head and tail */
#define MTK_MD_AP_QUERY_PATTERN		0x43434349	/* AP -> MD, head and tail */

#define MTK_MD_FEATURE_MASK		GENMASK(3, 0)
#define MTK_MD_FEATURE_VER		GENMASK(7, 4)

enum mtk_md_feature_support {
	MTK_MD_FEATURE_NOT_EXIST,
	MTK_MD_FEATURE_NOT_SUPPORT,
	MTK_MD_FEATURE_MUST,
	MTK_MD_FEATURE_OPTIONAL,
	MTK_MD_FEATURE_BACKWARD_COMPAT,
};

enum mtk_md_feature_id {
	MTK_MD_RT_BOOT_INFO			= 0,
	MTK_MD_RT_EXCEPTION_SHARE_MEMORY	= 1,
	MTK_MD_RT_CCIF_SHARE_MEMORY		= 2,
	MTK_MD_RT_SMART_LOGGING_SHARE_MEMORY	= 3,
	MTK_MD_RT_MD1MD3_SHARE_MEMORY		= 4,
	MTK_MD_RT_MISC_INFO_HIF_DMA_REMAP	= 5,
	MTK_MD_RT_MISC_INFO_RTC_32K_LESS	= 6,
	MTK_MD_RT_MISC_INFO_RANDOM_SEED_NUM	= 7,
	MTK_MD_RT_MISC_INFO_GPS_COCLOCK		= 8,
	MTK_MD_RT_MISC_INFO_SBP_ID		= 9,
	MTK_MD_RT_MISC_INFO_CCCI		= 10,
	MTK_MD_RT_MISC_INFO_CLIB_TIME		= 11,
	MTK_MD_RT_MISC_INFO_C2K			= 12,
	MTK_MD_RT_MD_IMAGE_START_MEMORY		= 13,
	MTK_MD_RT_CCISM_SHARE_MEMORY		= 14,
	MTK_MD_RT_CCB_SHARE_MEMORY		= 15,
	MTK_MD_RT_DHL_RAW_SHARE_MEMORY		= 16,
	MTK_MD_RT_DT_NETD_SHARE_MEMORY		= 17,
	MTK_MD_RT_DT_USB_SHARE_MEMORY		= 18,
	MTK_MD_RT_EE_AFTER_EPOF			= 19,
	MTK_MD_RT_AP_CCMNI_MTU			= 20,
	MTK_MD_RT_MISC_INFO_CUSTOMER_VAL	= 21,
	MTK_MD_RT_CCCI_FAST_HEADER		= 22,
	MTK_MD_RT_MISC_INFO_C2K_MEID		= 24,
	MTK_MD_RT_LWA_SHARE_MEMORY		= 25,
	MTK_MD_RT_AUDIO_RAW_SHARE_MEMORY	= 26,
	MTK_MD_RT_MULTI_MD_MPU			= 27,
	MTK_MD_RT_CCISM_SHARE_MEMORY_EXP	= 28,
	MTK_MD_RT_MD_PHY_CAPTURE		= 29,
	MTK_MD_RT_MD_CONSYS_SHARE_MEMORY	= 30,
	MTK_MD_RT_MD_MTEE_SMEM_ENABLE		= 32,
};

/* HS1's payload, as the MD leaves it in CCIF SRAM behind a CCCI header. */
struct mtk_md_md_query {
	__le32 head;
	u8 feature_set[MTK_MD_FEATURE_COUNT];
	__le32 tail;
	u8 reserved[152];
} __packed;

#define MTK_MD_HS1_LEN	(sizeof(struct mtk_md_ccci_hdr) + sizeof(struct mtk_md_md_query))

int mtk_md_hs1_check(const void *buf, size_t len, const struct mtk_md_md_query **query);

/* What the AP offers on generation 6293, as the vendor configures it. */
extern const u8 mtk_md_ap_features_6293[MTK_MD_FEATURE_COUNT];

int mtk_md_rt_negotiate(const u8 *md_set, const u8 *ap_set, u8 *out, unsigned int *bad_id);

/* One TLV of the runtime data: the 8-byte header, then data_len bytes. */
struct mtk_md_rt_feature {
	u8 feature_id;
	u8 support;
	u8 reserved[2];
	__le32 data_len;
} __packed;

/* The CCB control block (SMEM_USER_RAW_CCB_CTRL): one 64-byte entry per buffer config */
#define MTK_MD_SMEM_CCB_CTRL_OFFSET	(96 * SZ_1K)
#define MTK_MD_CCB_CTRL_WORDS		(20 * 16)
void mtk_md_ccb_ctrl_fill(__le32 *ctrl);

/* The modem's power budget block (SMEM_USER_RAW_DBM): guards, dBm tables, section levels */
#define MTK_MD_SMEM_DBM_OFFSET		(SZ_64K - MTK_MD_DBM_WORDS * 4)
#define MTK_MD_DBM_WORDS		44
void mtk_md_dbm_fill(__le32 *dbm);

int mtk_md_rt_append(u8 *buf, size_t size, size_t *pos, u8 id, u8 support, const void *data,
		     u32 data_len);

/*
 * The runtime data lives in the AP/MD1 non-cacheable share memory: the AP's TLVs in the first
 * 2 KiB of a 4 KiB region at 58 KiB, the modem's answer in the second. The modem addresses that
 * memory through its own window at 0x40000000.
 */
#define MTK_MD_SMEM_RUNTIME_OFFSET	(58 * 1024)
#define MTK_MD_SMEM_RUNTIME_AP_SIZE	0x800
#define MTK_MD_SMEM_RUNTIME_MD_SIZE	0x800

u32 mtk_md_smem_md_view(u64 ap_phys);

/* The AP's answer to HS1 (ap_query_md_feature_v2_1), behind a CCCI header in CCIF SRAM. */
struct mtk_md_ap_query {
	__le32 head;
	u8 feature_set[MTK_MD_FEATURE_COUNT];
	__le32 share_memory_support;
	__le32 ap_rt_addr;
	__le32 ap_rt_size;
	__le32 md_rt_addr;
	__le32 md_rt_size;
	__le32 noncached_mpu_start;
	__le32 noncached_mpu_size;
	__le32 cached_mpu_start;
	__le32 cached_mpu_size;
	__le32 reserved[12];
	__le32 tail;
} __packed;

void mtk_md_ap_query_fill(struct mtk_md_ap_query *q, u32 ap_rt_addr, u32 noncached_start,
			  u32 noncached_size, u32 cached_start, u32 cached_size);

/*
 * The CCIF ring queues (eccci/hif/ccci_ringbuf.c). A queue is one block of share memory: two
 * guard words, the control words of both directions, the receive area, the transmit area, two
 * guard words. "Receive" and "transmit" are the AP's view; the modem uses the same block with
 * the roles swapped. A message is framed by a marker and its length in front and two markers
 * behind, and takes a multiple of eight bytes.
 */
#define MTK_MD_RING_GUARD_HEAD		0xee0000ee
#define MTK_MD_RING_GUARD_TAIL		0xff0000ff
#define MTK_MD_RING_PKT_HEAD		0xaabbaabb
#define MTK_MD_RING_PKT_TAIL		0xccddeeff
#define MTK_MD_RING_PKT_OVERHEAD	16
#define MTK_MD_RING_QUEUES		8

struct mtk_md_ring {
	__le32 rx_read;
	__le32 rx_write;
	__le32 rx_length;
	__le32 tx_read;
	__le32 tx_write;
	__le32 tx_length;
	u8 buffer[];
};

#define MTK_MD_RING_CTL_LEN		(8 + sizeof(struct mtk_md_ring) + 8)

/* md1 on 6293: the normal queues fill SMEM_USER_CCISM_MCU, the exception queues _MCU_EXP */
extern const u32 mtk_md_ring_rx_size[MTK_MD_RING_QUEUES];
extern const u32 mtk_md_ring_tx_size[MTK_MD_RING_QUEUES];
extern const u32 mtk_md_ring_exp_size[MTK_MD_RING_QUEUES];	/* both directions */

struct mtk_md_ring *mtk_md_ring_create(void *buf, size_t buf_size, u32 rx_size, u32 tx_size,
				       size_t *used);
int mtk_md_ring_rx_peek(const struct mtk_md_ring *ring);
void mtk_md_ring_rx_read(const struct mtk_md_ring *ring, void *out, u32 len);
void mtk_md_ring_rx_consume(struct mtk_md_ring *ring, u32 len);
int mtk_md_ring_tx_write(struct mtk_md_ring *ring, const void *data, u32 len);

/*
 * RPC (eccci/port/port_rpc.c): the modem asks the AP for board facts. A request is the CCCI header,
 * an operation id, a parameter count and that many {u32 length, data padded to 4 bytes}. The answer
 * goes back on the paired channel with the operation id's top half set.
 */
/* ccci_config.h CCCI_MTU: payload bytes per message after the CCCI header */
#define MTK_MD_CCCI_MTU			(3584 - 128)
#define MTK_MD_CH_UART2_RX		10	/* AT responses and unsolicited results (ttyC0) */
#define MTK_MD_CH_UART2_TX		12	/* AT commands */
#define MTK_MD_CH_FS_RX			14	/* the modem's file requests */
#define MTK_MD_CH_FS_TX			15
#define MTK_MD_CH_RPC_RX		32
#define MTK_MD_CH_RPC_TX		33
#define MTK_MD_RPC_RESP			0xffff0000
#define MTK_MD_RPC_MAX_ARGS		6
#define MTK_MD_RPC_MAX_LEN		2048

struct mtk_md_rpc_req {
	u32 op;
	u32 argc;
	const u8 *arg[MTK_MD_RPC_MAX_ARGS];
	u32 arg_len[MTK_MD_RPC_MAX_ARGS];
};

int mtk_md_rpc_parse(const void *msg, size_t len, struct mtk_md_rpc_req *req);
int mtk_md_rpc_build(void *buf, size_t size, const struct mtk_md_ccci_hdr *req_hdr, u32 op,
		     u32 argc, const void *const *arg, const u32 *arg_len);

#endif /* __MTK_MD_PROTO_H__ */

// SPDX-License-Identifier: GPL-2.0-only
/*
 * HCI driver for the Bluetooth function of the MT6771 CONSYS radio.
 *
 * The vendor exposes this function as a character device for Android's
 * userspace stack. Here the same STP channel is registered with the kernel's
 * Bluetooth core instead.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/kernel_read_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "wmt_exp.h"
#include "stp_exp.h"

#define HCI_STP_RX_CHUNK	2048
#define HCI_STP_TX_RETRIES	3

/* the factory Bluetooth record: the address, most significant byte first, then radio settings */
#define HCI_STP_NVRAM_PATH	"/mnt/vendor/nvdata/APCFG/APRDEB/BT_Addr"
#define HCI_STP_OP_SET_BDADDR	0xfc1a

struct hci_stp {
	struct hci_dev *hdev;
	struct work_struct rx_work;
	struct sk_buff *rx_skb;
	unsigned int rx_need;
	bool rx_have_hdr;
	bool in_reset;
	u8 rx_chunk[HCI_STP_RX_CHUNK];
};

/* the STP and WMT callbacks carry no context pointer */
static struct hci_stp *hci_stp;

static int hci_stp_hdr_len(u8 type)
{
	switch (type) {
	case HCI_EVENT_PKT:
		return HCI_EVENT_HDR_SIZE;
	case HCI_ACLDATA_PKT:
		return HCI_ACL_HDR_SIZE;
	case HCI_SCODATA_PKT:
		return HCI_SCO_HDR_SIZE;
	case HCI_ISODATA_PKT:
		return HCI_ISO_HDR_SIZE;
	}
	return -EINVAL;
}

static unsigned int hci_stp_payload_len(struct sk_buff *skb)
{
	switch (hci_skb_pkt_type(skb)) {
	case HCI_EVENT_PKT:
		return hci_event_hdr(skb)->plen;
	case HCI_ACLDATA_PKT:
		return __le16_to_cpu(hci_acl_hdr(skb)->dlen);
	case HCI_SCODATA_PKT:
		return hci_sco_hdr(skb)->dlen;
	case HCI_ISODATA_PKT:
		return __le16_to_cpu(hci_iso_hdr(skb)->dlen) & 0x3fff;
	}
	return 0;
}

static void hci_stp_rx_drop(struct hci_stp *bt)
{
	kfree_skb(bt->rx_skb);
	bt->rx_skb = NULL;
}

/*
 * The firmware implements the LE 4.0 commands but leaves them out of its
 * supported commands bitmap, and the core unmasks the LE advertising and
 * connection events by that bitmap.
 */
static void hci_stp_fixup_commands(struct sk_buff *skb)
{
	static const u8 le_cmds[] = { 0xf7, 0xff, 0xff, 0x7f };
	struct hci_ev_cmd_complete *cc;
	struct hci_rp_read_local_commands *rp;
	unsigned int i;

	if (hci_skb_pkt_type(skb) != HCI_EVENT_PKT ||
	    hci_event_hdr(skb)->evt != HCI_EV_CMD_COMPLETE ||
	    skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cc) + sizeof(*rp))
		return;

	cc = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
	rp = (void *)(cc + 1);
	if (__le16_to_cpu(cc->opcode) != HCI_OP_READ_LOCAL_COMMANDS || rp->status)
		return;

	for (i = 0; i < ARRAY_SIZE(le_cmds); i++)
		rp->commands[25 + i] |= le_cmds[i];
}

/* STP hands over a byte stream: a packet may span two reads */
static void hci_stp_rx(struct hci_stp *bt, const u8 *data, unsigned int len)
{
	struct hci_dev *hdev = bt->hdev;
	unsigned int n;
	int hdr;

	while (len) {
		if (!bt->rx_skb) {
			hdr = hci_stp_hdr_len(*data);
			if (hdr < 0) {
				bt_dev_err(hdev, "unknown packet type 0x%02x, dropping %u bytes",
					   *data, len);
				hdev->stat.err_rx++;
				return;
			}
			bt->rx_skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_KERNEL);
			if (!bt->rx_skb) {
				hdev->stat.err_rx++;
				return;
			}
			hci_skb_pkt_type(bt->rx_skb) = *data;
			bt->rx_need = hdr;
			bt->rx_have_hdr = false;
			data++;
			len--;
			continue;
		}

		n = min(bt->rx_need, len);
		skb_put_data(bt->rx_skb, data, n);
		bt->rx_need -= n;
		data += n;
		len -= n;
		if (bt->rx_need)
			continue;

		if (!bt->rx_have_hdr) {
			bt->rx_have_hdr = true;
			bt->rx_need = hci_stp_payload_len(bt->rx_skb);
			if (bt->rx_need > skb_tailroom(bt->rx_skb)) {
				bt_dev_err(hdev, "oversized packet (%u bytes)", bt->rx_need);
				hdev->stat.err_rx++;
				hci_stp_rx_drop(bt);
				return;
			}
			if (bt->rx_need)
				continue;
		}

		hci_stp_fixup_commands(bt->rx_skb);
		hdev->stat.byte_rx += bt->rx_skb->len + 1;
		hci_recv_frame(hdev, bt->rx_skb);
		bt->rx_skb = NULL;
	}
}

static void hci_stp_rx_work(struct work_struct *work)
{
	struct hci_stp *bt = container_of(work, struct hci_stp, rx_work);
	int n;

	for (;;) {
		n = mtk_wcn_stp_receive_data(bt->rx_chunk, sizeof(bt->rx_chunk), BT_TASK_INDX);
		if (n <= 0)
			break;
		hci_stp_rx(bt, bt->rx_chunk, n);
	}
}

static void hci_stp_event_cb(void)
{
	if (hci_stp)
		schedule_work(&hci_stp->rx_work);
}

static void hci_stp_reset_cb(ENUM_WMTDRV_TYPE_T src, ENUM_WMTDRV_TYPE_T dst,
			     ENUM_WMTMSG_TYPE_T type, void *buf, unsigned int sz)
{
	struct hci_stp *bt = hci_stp;
	ENUM_WMTRSTMSG_TYPE_T msg = 0;

	if (!bt || sz > sizeof(msg))
		return;
	if (src != WMTDRV_TYPE_WMT || dst != WMTDRV_TYPE_BT || type != WMTMSG_TYPE_RESET)
		return;

	memcpy(&msg, buf, sz);
	switch (msg) {
	case WMTRSTMSG_RESET_START:
		bt_dev_warn(bt->hdev, "whole chip reset");
		bt->in_reset = true;
		break;
	case WMTRSTMSG_RESET_END:
	case WMTRSTMSG_RESET_END_FAIL:
		bt->in_reset = false;
		hci_reset_dev(bt->hdev);
		break;
	default:
		break;
	}
}

static int hci_stp_open(struct hci_dev *hdev)
{
	struct hci_stp *bt = hci_get_drvdata(hdev);

	if (mtk_wcn_wmt_func_on(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "WMT could not turn the Bluetooth function on");
		return -EIO;
	}
	if (mtk_wcn_stp_is_ready() == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "STP is not ready");
		mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT);
		return -EIO;
	}

	/* deliver through the STP receive queue, as the vendor driver does */
	mtk_wcn_stp_set_bluez(0);
	bt->in_reset = false;
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, hci_stp_event_cb);
	mtk_wcn_wmt_msgcb_reg(WMTDRV_TYPE_BT, hci_stp_reset_cb);
	return 0;
}

static int hci_stp_close(struct hci_dev *hdev)
{
	struct hci_stp *bt = hci_get_drvdata(hdev);

	mtk_wcn_wmt_msgcb_unreg(WMTDRV_TYPE_BT);
	mtk_wcn_stp_register_event_cb(BT_TASK_INDX, NULL);
	cancel_work_sync(&bt->rx_work);
	hci_stp_rx_drop(bt);

	if (mtk_wcn_wmt_func_off(WMTDRV_TYPE_BT) == MTK_WCN_BOOL_FALSE) {
		bt_dev_err(hdev, "WMT could not turn the Bluetooth function off");
		return -EIO;
	}
	return 0;
}

static int hci_stp_flush(struct hci_dev *hdev)
{
	return 0;
}

static int hci_stp_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct hci_stp *bt = hci_get_drvdata(hdev);
	int retry, n = 0;

	if (bt->in_reset)
		return -EIO;

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	}

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	/* 0 means the STP window is full: the vendor driver waits and retries */
	for (retry = 0; retry < HCI_STP_TX_RETRIES; retry++) {
		if (retry)
			msleep(30);
		n = mtk_wcn_stp_send_data(skb->data, skb->len, BT_TASK_INDX);
		if (n)
			break;
	}
	if (n <= 0) {
		hdev->stat.err_tx++;
		return n ? -EIO : -EAGAIN;
	}

	hdev->stat.byte_tx += skb->len;
	kfree_skb(skb);
	return 0;
}

static int hci_stp_set_bdaddr(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;

	skb = __hci_cmd_sync(hdev, HCI_STP_OP_SET_BDADDR, sizeof(*bdaddr), bdaddr,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	kfree_skb(skb);
	return 0;
}

/* without the factory address the controller answers to a default shared by every unit */
static int hci_stp_setup(struct hci_dev *hdev)
{
	void *data = NULL;
	size_t fsize = 0;
	bdaddr_t bdaddr;
	ssize_t n;
	int ret;

	n = kernel_read_file_from_path_initns(HCI_STP_NVRAM_PATH, 0, &data, sizeof(bdaddr),
					      &fsize, READING_FIRMWARE);
	if (n != sizeof(bdaddr)) {
		bt_dev_warn(hdev, "no factory address (%s: %zd)", HCI_STP_NVRAM_PATH, n);
		if (n >= 0)
			vfree(data);
		return 0;
	}
	baswap(&bdaddr, data);
	vfree(data);

	if (!bacmp(&bdaddr, BDADDR_ANY) || !bacmp(&bdaddr, BDADDR_NONE)) {
		bt_dev_warn(hdev, "factory address is not set");
		return 0;
	}

	ret = hci_stp_set_bdaddr(hdev, &bdaddr);
	if (ret)
		bt_dev_warn(hdev, "setting the factory address failed: %d", ret);
	return 0;
}

static int __init hci_stp_init(void)
{
	struct hci_stp *bt;
	struct hci_dev *hdev;
	int ret;

	bt = kzalloc_obj(*bt);
	if (!bt)
		return -ENOMEM;

	hdev = hci_alloc_dev();
	if (!hdev) {
		kfree(bt);
		return -ENOMEM;
	}

	INIT_WORK(&bt->rx_work, hci_stp_rx_work);
	bt->hdev = hdev;
	hci_set_drvdata(hdev, bt);

	hdev->bus = HCI_UART;
	hdev->open = hci_stp_open;
	hdev->close = hci_stp_close;
	hdev->flush = hci_stp_flush;
	hdev->send = hci_stp_send;
	hdev->setup = hci_stp_setup;
	hdev->set_bdaddr = hci_stp_set_bdaddr;

	hci_stp = bt;
	ret = hci_register_dev(hdev);
	if (ret < 0) {
		hci_stp = NULL;
		hci_free_dev(hdev);
		kfree(bt);
		return ret;
	}
	return 0;
}

static void __exit hci_stp_exit(void)
{
	struct hci_stp *bt = hci_stp;

	hci_unregister_dev(bt->hdev);
	hci_stp = NULL;
	hci_free_dev(bt->hdev);
	kfree(bt);
}

module_init(hci_stp_init);
module_exit(hci_stp_exit);

MODULE_DESCRIPTION("MT6771 CONSYS Bluetooth HCI driver over STP");
MODULE_LICENSE("GPL");

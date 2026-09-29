// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek connsys/modem message bridge, in place of the vendor's conn_md (drivers/misc/mediatek/
 * conn_md in the MT6771 4.4 BSP). The modem driver registers the modem module it relays for, the
 * WMT core its AP task; a message goes to the user registered for its destination, and only if
 * its source is registered too. Messages are copied and delivered in order, one at a time, from a
 * single worker: WMT's handler waits for the connsys firmware and must not hold up the modem's
 * receive path. A handler runs with the user list locked, so unregistering waits for it.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk_conn_md.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

struct mtk_conn_md_user {
	struct list_head node;
	u32 id;
	int (*rx_cb)(struct mtk_conn_md_ilm *ilm);
};

struct mtk_conn_md_msg {
	struct list_head node;
	struct mtk_conn_md_ilm ilm;
	u8 para[];
};

static LIST_HEAD(mtk_conn_md_users);
static DEFINE_MUTEX(mtk_conn_md_users_lock);
static LIST_HEAD(mtk_conn_md_queue);
static DEFINE_SPINLOCK(mtk_conn_md_queue_lock);
static struct workqueue_struct *mtk_conn_md_wq;
static void mtk_conn_md_deliver(struct work_struct *work);
static DECLARE_WORK(mtk_conn_md_work, mtk_conn_md_deliver);

static struct mtk_conn_md_user *mtk_conn_md_find(u32 id)
{
	struct mtk_conn_md_user *u;

	list_for_each_entry(u, &mtk_conn_md_users, node)
		if (u->id == id)
			return u;
	return NULL;
}

/**
 * mtk_conn_md_bridge_reg() - receive the messages addressed to @id
 *
 * A second registration of the same id replaces the handler, as in the vendor bridge.
 */
int mtk_conn_md_bridge_reg(u32 id, const struct mtk_conn_md_ops *ops)
{
	struct mtk_conn_md_user *u;

	if (!ops || !ops->rx_cb)
		return -EINVAL;

	mutex_lock(&mtk_conn_md_users_lock);
	u = mtk_conn_md_find(id);
	if (!u) {
		u = kzalloc_obj(*u, GFP_KERNEL);
		if (!u) {
			mutex_unlock(&mtk_conn_md_users_lock);
			return -ENOMEM;
		}
		u->id = id;
		list_add_tail(&u->node, &mtk_conn_md_users);
	} else {
		pr_warn("id %#x registered again, handler replaced\n", id);
	}
	u->rx_cb = ops->rx_cb;
	mutex_unlock(&mtk_conn_md_users_lock);
	pr_info("id %#x registered\n", id);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(mtk_conn_md_bridge_reg, "MTK_CONN_MD");

/**
 * mtk_conn_md_bridge_unreg() - stop receiving for @id
 *
 * Returns once no handler of @id runs any more; messages still queued to or from @id are dropped.
 */
int mtk_conn_md_bridge_unreg(u32 id)
{
	struct mtk_conn_md_msg *m, *tmp;
	struct mtk_conn_md_user *u;
	LIST_HEAD(drop);

	mutex_lock(&mtk_conn_md_users_lock);
	u = mtk_conn_md_find(id);
	if (u)
		list_del(&u->node);
	mutex_unlock(&mtk_conn_md_users_lock);
	kfree(u);

	spin_lock(&mtk_conn_md_queue_lock);
	list_for_each_entry_safe(m, tmp, &mtk_conn_md_queue, node)
		if (m->ilm.src_mod_id == id || m->ilm.dest_mod_id == id)
			list_move_tail(&m->node, &drop);
	spin_unlock(&mtk_conn_md_queue_lock);
	list_for_each_entry_safe(m, tmp, &drop, node)
		kfree(m);

	return u ? 0 : -ENOENT;
}
EXPORT_SYMBOL_NS_GPL(mtk_conn_md_bridge_unreg, "MTK_CONN_MD");

/**
 * mtk_conn_md_bridge_send_msg() - queue a copy of @ilm and its local_para block
 *
 * May sleep. Whether the message reaches anyone is decided when it is delivered.
 */
int mtk_conn_md_bridge_send_msg(const struct mtk_conn_md_ilm *ilm)
{
	struct mtk_conn_md_msg *m;
	u16 len;

	if (!ilm || !ilm->local_para_ptr)
		return -EINVAL;
	len = ilm->local_para_ptr->msg_len;
	if (len < sizeof(struct mtk_conn_md_para))
		return -EINVAL;

	m = kmalloc_flex(*m, para, len, GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->ilm = *ilm;
	memcpy(m->para, ilm->local_para_ptr, len);
	m->ilm.local_para_ptr = (struct mtk_conn_md_para *)m->para;
	m->ilm.peer_buff_ptr = NULL;

	spin_lock(&mtk_conn_md_queue_lock);
	list_add_tail(&m->node, &mtk_conn_md_queue);
	spin_unlock(&mtk_conn_md_queue_lock);
	queue_work(mtk_conn_md_wq, &mtk_conn_md_work);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(mtk_conn_md_bridge_send_msg, "MTK_CONN_MD");

static void mtk_conn_md_deliver(struct work_struct *work)
{
	struct mtk_conn_md_user *dest;
	struct mtk_conn_md_msg *m;

	for (;;) {
		spin_lock(&mtk_conn_md_queue_lock);
		m = list_first_entry_or_null(&mtk_conn_md_queue, struct mtk_conn_md_msg, node);
		if (m)
			list_del(&m->node);
		spin_unlock(&mtk_conn_md_queue_lock);
		if (!m)
			return;

		mutex_lock(&mtk_conn_md_users_lock);
		dest = mtk_conn_md_find(m->ilm.dest_mod_id);
		if (!mtk_conn_md_find(m->ilm.src_mod_id))
			pr_warn_ratelimited("message %#x from unregistered id %#x dropped\n",
					    m->ilm.msg_id, m->ilm.src_mod_id);
		else if (!dest)
			pr_warn_ratelimited("message %#x to unregistered id %#x dropped\n",
					    m->ilm.msg_id, m->ilm.dest_mod_id);
		else
			dest->rx_cb(&m->ilm);
		mutex_unlock(&mtk_conn_md_users_lock);
		kfree(m);
	}
}

static int __init mtk_conn_md_init(void)
{
	mtk_conn_md_wq = alloc_ordered_workqueue("mtk_conn_md", 0);
	return mtk_conn_md_wq ? 0 : -ENOMEM;
}
module_init(mtk_conn_md_init);

/* every user holds a reference on this module, so none is left here */
static void __exit mtk_conn_md_exit(void)
{
	struct mtk_conn_md_msg *m, *tmp;

	destroy_workqueue(mtk_conn_md_wq);
	list_for_each_entry_safe(m, tmp, &mtk_conn_md_queue, node)
		kfree(m);
}
module_exit(mtk_conn_md_exit);

MODULE_DESCRIPTION("MediaTek connsys/modem message bridge (LTE coexistence)");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier: GPL-2.0
/*
 * Bring-up aid of the MediaTek CAMSV driver: status dump, raw register access
 * and a list of register writes applied before the timing generator starts.
 * To be removed or trimmed once the register layout is settled.
 *
 * The registers of the block hang the SoC when the power domain or a clock is
 * off, so every access below holds a runtime PM reference taken with
 * pm_runtime_get_if_active() and the files print "off" otherwise.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "mtk-camsv.h"

static const struct {
	u32 bit;
	const char *name;
} mtk_camsv_dbg_int_names[] = {
	{ CAMSV_INT_VS1, "vs1" },
	{ CAMSV_INT_TG_ST1, "tg_st1" },
	{ CAMSV_INT_TG_ST2, "tg_st2" },
	{ CAMSV_INT_EXPDON, "expdon" },
	{ CAMSV_INT_TG_ERR, "tg_err" },
	{ CAMSV_INT_TG_GBERR, "tg_gberr" },
	{ CAMSV_INT_SOF, "sof" },
	{ CAMSV_INT_HW_PASS1_DON, "hw_pass1_don" },
	{ CAMSV_INT_IMGO_ERR, "imgo_err" },
	{ CAMSV_INT_IMGO_OVERRUN, "imgo_overrun" },
	{ CAMSV_INT_SW_PASS1_DON, "sw_pass1_don" },
};

static bool mtk_camsv_dbg_get(struct mtk_camsv *priv)
{
	return pm_runtime_get_if_active(priv->dev) > 0;
}

static void mtk_camsv_dbg_put(struct mtk_camsv *priv)
{
	pm_runtime_put(priv->dev);
}

/* status */

static int mtk_camsv_dbg_status_show(struct seq_file *s, void *unused)
{
	struct mtk_camsv *priv = s->private;
	const struct mtk_camsv_stats *st = &priv->stats;
	unsigned long flags;
	unsigned int i;
	u32 inter;

	if (!mtk_camsv_dbg_get(priv)) {
		seq_puts(s, "off\n");
		return 0;
	}

	inter = camsv_read(priv, CAMSV_TG_INTER_ST);
	seq_printf(s, "streaming: %s, dma %s\n", priv->streaming ? "yes" : "no",
		   priv->dbg_dma ? "on" : "off");
	seq_printf(s, "TG: SEN_MODE %08x VF_CON %08x GRAB_PXL %08x GRAB_LIN %08x\n",
		   camsv_read(priv, CAMSV_TG_SEN_MODE), camsv_read(priv, CAMSV_TG_VF_CON),
		   camsv_read(priv, CAMSV_TG_SEN_GRAB_PXL), camsv_read(priv, CAMSV_TG_SEN_GRAB_LIN));
	seq_printf(s, "TG: INTER_ST %08x state %lu frames %lu FRMSIZE_ST %08x\n", inter,
		   FIELD_GET(CAMSV_TG_INTER_ST_STATE, inter),
		   FIELD_GET(CAMSV_TG_INTER_ST_FRAMES, inter),
		   camsv_read(priv, CAMSV_TG_FRMSIZE_ST));
	/* INT_STATUS is read to clear: leave it to the interrupt handler */
	seq_printf(s, "top: MODULE_EN %08x FMT_SEL %08x INT_EN %08x SW_CTL %08x PAK %08x\n",
		   camsv_read(priv, CAMSV_MODULE_EN), camsv_read(priv, CAMSV_FMT_SEL),
		   camsv_read(priv, CAMSV_INT_EN), camsv_read(priv, CAMSV_SW_CTL),
		   camsv_read(priv, CAMSV_PAK));
	seq_printf(s, "dma: BASE_ADDR %08x XSIZE %08x YSIZE %08x STRIDE %08x\n",
		   camsv_read(priv, CAMSV_IMGO_BASE_ADDR), camsv_read(priv, CAMSV_IMGO_XSIZE),
		   camsv_read(priv, CAMSV_IMGO_YSIZE), camsv_read(priv, CAMSV_IMGO_STRIDE));
	seq_printf(s, "fbc: CTL1 %08x CTL2 %08x\n", camsv_read(priv, CAMSV_FBC_IMGO_CTL1),
		   camsv_read(priv, CAMSV_FBC_IMGO_CTL2));

	spin_lock_irqsave(&priv->qlock, flags);
	seq_printf(s, "irq: total %u frames %u drops %u sequence %u\n",
		   st->irqs, st->frames, st->drops, st->sequence);
	for (i = 0; i < 32; i++) {
		const char *name = "?";
		unsigned int j;

		if (!st->bits[i])
			continue;
		for (j = 0; j < ARRAY_SIZE(mtk_camsv_dbg_int_names); j++)
			if (mtk_camsv_dbg_int_names[j].bit == BIT(i))
				name = mtk_camsv_dbg_int_names[j].name;
		seq_printf(s, "  bit %u %s: %u\n", i, name, st->bits[i]);
	}
	spin_unlock_irqrestore(&priv->qlock, flags);

	mtk_camsv_dbg_put(priv);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_camsv_dbg_status);

/* power: hold the power domain and the clocks so that regs can be read */

static ssize_t mtk_camsv_dbg_power_write(struct file *file, const char __user *ubuf,
					 size_t len, loff_t *ppos)
{
	struct mtk_camsv *priv = file_inode(file)->i_private;
	bool on;
	int ret;

	ret = kstrtobool_from_user(ubuf, len, &on);
	if (ret)
		return ret;

	mutex_lock(&priv->lock);
	if (on && !priv->dbg_power) {
		ret = pm_runtime_resume_and_get(priv->dev);
		if (!ret)
			priv->dbg_power = true;
	} else if (!on && priv->dbg_power) {
		pm_runtime_put(priv->dev);
		priv->dbg_power = false;
	}
	mutex_unlock(&priv->lock);

	return ret ?: len;
}

static ssize_t mtk_camsv_dbg_power_read(struct file *file, char __user *ubuf,
					size_t len, loff_t *ppos)
{
	struct mtk_camsv *priv = file_inode(file)->i_private;
	char buf[3] = { priv->dbg_power ? '1' : '0', '\n', 0 };

	return simple_read_from_buffer(ubuf, len, ppos, buf, 2);
}

static const struct file_operations mtk_camsv_dbg_power_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_camsv_dbg_power_read,
	.write = mtk_camsv_dbg_power_write,
};

/* regs: dump the page; write "off" to read one register or "off val" to write it */

static int mtk_camsv_dbg_regs_show(struct seq_file *s, void *unused)
{
	struct mtk_camsv *priv = s->private;
	unsigned int off, i;

	if (!mtk_camsv_dbg_get(priv)) {
		seq_puts(s, "off\n");
		return 0;
	}

	for (off = 0; off < CAMSV_PAGE_SIZE; off += 16) {
		seq_printf(s, "%04x:", off);
		for (i = 0; i < 4; i++) {
			/* Read to clear, and the handler needs the bits */
			if (priv->streaming && off + 4 * i == CAMSV_INT_STATUS)
				seq_puts(s, " --------");
			else
				seq_printf(s, " %08x", camsv_read(priv, off + 4 * i));
		}
		seq_putc(s, '\n');
	}

	mtk_camsv_dbg_put(priv);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_camsv_dbg_regs);

static ssize_t mtk_camsv_dbg_reg_write(struct file *file, const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct mtk_camsv *priv = file_inode(file)->i_private;
	unsigned int off, val;
	char buf[40];
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	n = sscanf(buf, "%i %i", &off, &val);
	if (n < 1 || off >= CAMSV_PAGE_SIZE || off & 3)
		return -EINVAL;

	if (!mtk_camsv_dbg_get(priv))
		return -EAGAIN;
	if (n == 2)
		camsv_write(priv, off, val);
	priv->dbg_reg_val = camsv_read(priv, off);
	mtk_camsv_dbg_put(priv);

	return len;
}

static ssize_t mtk_camsv_dbg_reg_read(struct file *file, char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct mtk_camsv *priv = file_inode(file)->i_private;
	char buf[16];
	int n = scnprintf(buf, sizeof(buf), "%08x\n", priv->dbg_reg_val);

	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations mtk_camsv_dbg_reg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_camsv_dbg_reg_read,
	.write = mtk_camsv_dbg_reg_write,
};

/* extra: "off val [mask]" adds a write for the next stream start, "clear" empties the list */

static ssize_t mtk_camsv_dbg_extra_write(struct file *file, const char __user *ubuf,
					 size_t len, loff_t *ppos)
{
	struct mtk_camsv *priv = file_inode(file)->i_private;
	unsigned int off, val, mask = ~0;
	char buf[48];
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	mutex_lock(&priv->lock);
	if (sysfs_streq(buf, "clear")) {
		priv->num_extra = 0;
		mutex_unlock(&priv->lock);
		return len;
	}

	n = sscanf(buf, "%i %i %i", &off, &val, &mask);
	if (n < 2 || off >= CAMSV_PAGE_SIZE || off & 3 || priv->num_extra >= CAMSV_MAX_EXTRA) {
		mutex_unlock(&priv->lock);
		return -EINVAL;
	}

	priv->extra[priv->num_extra++] = (struct mtk_camsv_extra){ off, mask, val };
	mutex_unlock(&priv->lock);

	return len;
}

static int mtk_camsv_dbg_extra_show(struct seq_file *s, void *unused)
{
	struct mtk_camsv *priv = s->private;
	unsigned int i;

	mutex_lock(&priv->lock);
	for (i = 0; i < priv->num_extra; i++)
		seq_printf(s, "%04x %08x mask %08x\n", priv->extra[i].off, priv->extra[i].val,
			   priv->extra[i].mask);
	mutex_unlock(&priv->lock);

	return 0;
}

static int mtk_camsv_dbg_extra_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtk_camsv_dbg_extra_show, inode->i_private);
}

static const struct file_operations mtk_camsv_dbg_extra_fops = {
	.owner = THIS_MODULE,
	.open = mtk_camsv_dbg_extra_open,
	.read = seq_read,
	.write = mtk_camsv_dbg_extra_write,
	.llseek = seq_lseek,
	.release = single_release,
};

void mtk_camsv_debugfs_init(struct mtk_camsv *priv)
{
	struct dentry *dir = debugfs_create_dir("mtk-camsv", NULL);

	priv->debugfs = dir;

	debugfs_create_file("status", 0444, dir, priv, &mtk_camsv_dbg_status_fops);
	debugfs_create_file("power", 0644, dir, priv, &mtk_camsv_dbg_power_fops);
	debugfs_create_file("regs", 0444, dir, priv, &mtk_camsv_dbg_regs_fops);
	debugfs_create_file("reg", 0644, dir, priv, &mtk_camsv_dbg_reg_fops);
	debugfs_create_file("extra", 0644, dir, priv, &mtk_camsv_dbg_extra_fops);
	/* Applied by the next stream on */
	debugfs_create_bool("dma", 0644, dir, &priv->dbg_dma);
	debugfs_create_x32("int_en", 0644, dir, &priv->dbg_int_en);
	/* Byte the buffers are filled with when they are queued, 0 = leave them */
	debugfs_create_x32("poison", 0644, dir, &priv->dbg_poison);
}

void mtk_camsv_debugfs_exit(struct mtk_camsv *priv)
{
	debugfs_remove_recursive(priv->debugfs);

	mutex_lock(&priv->lock);
	if (priv->dbg_power) {
		pm_runtime_put(priv->dev);
		priv->dbg_power = false;
	}
	mutex_unlock(&priv->lock);
}

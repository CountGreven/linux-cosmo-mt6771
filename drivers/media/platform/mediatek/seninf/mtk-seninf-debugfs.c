// SPDX-License-Identifier: GPL-2.0
/*
 * Bring-up aid of the MediaTek SENINF receiver: status dump, tunables and a
 * way to start a stream without a capture driver. To be removed or trimmed
 * once the receiver is settled.
 *
 * Registers of the block hang the SoC when the power domain or the clocks are
 * off, so every access below holds a runtime PM reference taken with
 * pm_runtime_get_if_active() and the files print "off" otherwise.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "mtk-seninf.h"

#define SENINF_DBG_PKT_SAMPLES	4

static const struct {
	unsigned int page;
	const char *ports;
} mtk_seninf_dbg_pages[] = {
	{ 0, "csi0, csi0a" },
	{ 1, "csi0b" },
	{ 2, "csi1" },
	{ 4, "csi2" },
};

/* Take a reference on the powered block, or 0 if it is off */
static bool mtk_seninf_dbg_get(struct mtk_seninf *priv)
{
	return pm_runtime_get_if_active(priv->dev) > 0;
}

static void mtk_seninf_dbg_put(struct mtk_seninf *priv)
{
	pm_runtime_put(priv->dev);
}

/*
 * The packet counter is narrow and wraps many times per frame, so only
 * whether it moves between samples a millisecond apart says something.
 */
static const char *mtk_seninf_dbg_counting(struct mtk_seninf *priv,
					   unsigned int page)
{
	u32 first = seninf_read(priv, page, SENINF_CSI2_DBG_PORT);
	unsigned int i;

	if (seninf_read(priv, page, SENINF_CSI2_DGB_SEL) != SENINF_CSI2_DGB_SEL_PKT)
		return "n/a";

	for (i = 1; i < SENINF_DBG_PKT_SAMPLES; i++) {
		usleep_range(900, 1100);
		if (seninf_read(priv, page, SENINF_CSI2_DBG_PORT) != first)
			return "yes";
	}

	return "no";
}

static void mtk_seninf_dbg_status_page(struct seq_file *s, struct mtk_seninf *priv,
				       unsigned int page, const char *ports)
{
	const char *counting = mtk_seninf_dbg_counting(priv, page);
	u32 intr = seninf_read(priv, page, SENINF_CSI2_INT_STATUS);
	u32 intr_ext = seninf_read(priv, page, SENINF_CSI2_INT_STATUS_EXT);

	seq_printf(s, "page %u (%s): CTRL %08x CSI2_CTL %08x LNRD_TIMING %08x CON24 %08x\n",
		   page, ports, seninf_read(priv, page, SENINF_CTRL),
		   seninf_read(priv, page, SENINF_CSI2_CTL),
		   seninf_read(priv, page, SENINF_CSI2_LNRD_TIMING),
		   seninf_read(priv, page, SENINF_MIPI_RX_CON24));
	seq_printf(s, "  packets: DGB_SEL %08x DBG_PORT %08x counting %s\n",
		   seninf_read(priv, page, SENINF_CSI2_DGB_SEL),
		   seninf_read(priv, page, SENINF_CSI2_DBG_PORT), counting);
	seq_printf(s, "  state: LNRC_FSM %08x LNRD_FSM %08x HSRX_DBG %08x FRAME_LINE_NUM %08x\n",
		   seninf_read(priv, page, SENINF_CSI2_LNRC_FSM),
		   seninf_read(priv, page, SENINF_CSI2_LNRD_FSM),
		   seninf_read(priv, page, SENINF_CSI2_HSRX_DBG),
		   seninf_read(priv, page, SENINF_CSI2_FRAME_LINE_NUM));
	seq_printf(s, "  errors: INT_STATUS %08x INT_STATUS_EXT %08x fatal %02lx (cleared by this read)\n",
		   intr, intr_ext, intr & SENINF_CSI2_INT_ERR_MASK);

	seninf_write(priv, page, SENINF_CSI2_INT_STATUS, intr);
	seninf_write(priv, page, SENINF_CSI2_INT_STATUS_EXT, intr_ext);
}

static int mtk_seninf_dbg_status_show(struct seq_file *s, void *unused)
{
	struct mtk_seninf *priv = s->private;
	u32 size, mux_int;
	unsigned int i;

	mutex_lock(&priv->lock);

	if (!mtk_seninf_dbg_get(priv)) {
		seq_puts(s, "off\n");
		goto unlock;
	}

	if (priv->streaming)
		seq_printf(s, "streaming: %s from port %u on receiver port %s\n",
			   priv->stream_sensor->name, priv->stream_port,
			   mtk_seninf_port_info[priv->stream_hw_port].phy_name);
	else
		seq_puts(s, "streaming: no\n");
	seq_printf(s, "test model: %s\n", priv->tm_on ? "on" : "off");
	seq_printf(s, "knobs: settle %#x hs_trail %#x port %#x lanes %u lane_map %#x\n",
		   priv->tune.settle, priv->tune.hs_trail, priv->tune.port,
		   priv->tune.lanes, priv->tune.lane_map);
	seq_printf(s, "top: MUX_CTRL %08x CAM_MUX_CTRL %08x PHY_CTL csi0 %08x csi1 %08x csi2 %08x\n",
		   seninf_read(priv, 0, SENINF_TOP_MUX_CTRL),
		   seninf_read(priv, 0, SENINF_TOP_CAM_MUX_CTRL),
		   seninf_read(priv, 0, SENINF_TOP_PHY_CTL_CSI(0)),
		   seninf_read(priv, 0, SENINF_TOP_PHY_CTL_CSI(1)),
		   seninf_read(priv, 0, SENINF_TOP_PHY_CTL_CSI(2)));

	for (i = 0; i < ARRAY_SIZE(mtk_seninf_dbg_pages); i++)
		mtk_seninf_dbg_status_page(s, priv, mtk_seninf_dbg_pages[i].page,
					   mtk_seninf_dbg_pages[i].ports);

	/* MUX_SIZE is a plain register that nothing writes, the measurement is DEBUG_2 */
	size = seninf_read(priv, SENINF_MUX, SENINF_MUX_DEBUG_2);
	mux_int = seninf_read(priv, SENINF_MUX, SENINF_MUX_INTSTA);
	seq_printf(s, "mux0: CTRL %08x measured %u x %u (DEBUG_2 %08x) DEBUG_3 %08x INTSTA %08x\n",
		   seninf_read(priv, SENINF_MUX, SENINF_MUX_CTRL),
		   size >> 16, size & 0xffff, size,
		   seninf_read(priv, SENINF_MUX, SENINF_MUX_DEBUG_3), mux_int);
	seninf_write(priv, SENINF_MUX, SENINF_MUX_INTSTA, mux_int);

	mtk_seninf_dbg_put(priv);
unlock:
	mutex_unlock(&priv->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mtk_seninf_dbg_status);

/* power: hold the power domain and the clocks so that regs can be read */

static ssize_t mtk_seninf_dbg_power_write(struct file *file, const char __user *ubuf,
					  size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
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

static ssize_t mtk_seninf_dbg_power_read(struct file *file, char __user *ubuf,
					 size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
	char buf[3] = { priv->dbg_power ? '1' : '0', '\n', 0 };

	return simple_read_from_buffer(ubuf, len, ppos, buf, 2);
}

static const struct file_operations mtk_seninf_dbg_power_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_seninf_dbg_power_read,
	.write = mtk_seninf_dbg_power_write,
};

/* regs: raw dump of one SENINF page, written with the page number */

static int mtk_seninf_dbg_regs_show(struct seq_file *s, void *unused)
{
	struct mtk_seninf *priv = s->private;
	unsigned int off;

	mutex_lock(&priv->lock);

	if (!mtk_seninf_dbg_get(priv)) {
		seq_puts(s, "off\n");
		goto unlock;
	}

	seq_printf(s, "page %u\n", priv->dbg_page);
	for (off = 0; off < SENINF_PAGE_SIZE; off += 16)
		seq_printf(s, "%04x: %08x %08x %08x %08x\n", off,
			   seninf_read(priv, priv->dbg_page, off),
			   seninf_read(priv, priv->dbg_page, off + 4),
			   seninf_read(priv, priv->dbg_page, off + 8),
			   seninf_read(priv, priv->dbg_page, off + 12));

	mtk_seninf_dbg_put(priv);
unlock:
	mutex_unlock(&priv->lock);
	return 0;
}

static int mtk_seninf_dbg_regs_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtk_seninf_dbg_regs_show, inode->i_private);
}

static ssize_t mtk_seninf_dbg_regs_write(struct file *file, const char __user *ubuf,
					 size_t len, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct mtk_seninf *priv = s->private;
	u32 page;
	int ret;

	ret = kstrtou32_from_user(ubuf, len, 0, &page);
	if (ret)
		return ret;
	if (page >= SENINF_NUM_PAGES)
		return -EINVAL;

	priv->dbg_page = page;

	return len;
}

static const struct file_operations mtk_seninf_dbg_regs_fops = {
	.owner = THIS_MODULE,
	.open = mtk_seninf_dbg_regs_open,
	.read = seq_read,
	.write = mtk_seninf_dbg_regs_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/* reg: read or write one register of a page: "page off" reads, "page off val" writes */

static ssize_t mtk_seninf_dbg_reg_write(struct file *file, const char __user *ubuf,
					size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
	unsigned int page, off, val;
	char buf[48];
	int n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	n = sscanf(buf, "%i %i %i", &page, &off, &val);
	if (n < 2 || page >= SENINF_NUM_PAGES || off >= SENINF_PAGE_SIZE || off & 3)
		return -EINVAL;

	mutex_lock(&priv->lock);
	if (!mtk_seninf_dbg_get(priv)) {
		mutex_unlock(&priv->lock);
		return -EAGAIN;
	}
	if (n == 3)
		seninf_write(priv, page, off, val);
	priv->dbg_reg_val = seninf_read(priv, page, off);
	mtk_seninf_dbg_put(priv);
	mutex_unlock(&priv->lock);

	return len;
}

static ssize_t mtk_seninf_dbg_reg_read(struct file *file, char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
	char buf[16];
	int n = scnprintf(buf, sizeof(buf), "%08x\n", priv->dbg_reg_val);

	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations mtk_seninf_dbg_reg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_seninf_dbg_reg_read,
	.write = mtk_seninf_dbg_reg_write,
};

/* stream: bring-up only, start and stop a sensor without a capture driver */

static ssize_t mtk_seninf_dbg_stream_write(struct file *file, const char __user *ubuf,
					   size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
	unsigned int port;
	char buf[16];
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	if (sysfs_streq(buf, "off")) {
		mtk_seninf_stop(priv);
		return len;
	}

	if (sscanf(buf, "on %u", &port) != 1)
		return -EINVAL;

	ret = mtk_seninf_start(priv, port);

	return ret ?: len;
}

static ssize_t mtk_seninf_dbg_stream_read(struct file *file, char __user *ubuf,
					  size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
	char buf[80];
	int n;

	mutex_lock(&priv->lock);
	if (priv->streaming)
		n = scnprintf(buf, sizeof(buf), "on %u (receiver port %s)\n",
			      priv->stream_port,
			      mtk_seninf_port_info[priv->stream_hw_port].phy_name);
	else
		n = scnprintf(buf, sizeof(buf), "off\n");
	mutex_unlock(&priv->lock);

	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations mtk_seninf_dbg_stream_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_seninf_dbg_stream_read,
	.write = mtk_seninf_dbg_stream_write,
};

/* tm: the internal test model into MUX 0, no sensor and no analog needed */

static void mtk_seninf_dbg_tm_stop(struct mtk_seninf *priv)
{
	if (!priv->tm_on)
		return;

	seninf_write(priv, 0, SENINF_TG_TM_CTL, 0);
	seninf_update(priv, 0, SENINF_CTRL_EXT, SENINF_CTRL_EXT_TM, 0);
	seninf_update(priv, SENINF_MUX, SENINF_MUX_CTRL, SENINF_MUX_CTRL_EN, 0);
	pm_runtime_put(priv->dev);
	priv->tm_on = false;
}

static int mtk_seninf_dbg_tm_start(struct mtk_seninf *priv, u32 width, u32 height)
{
	int ret;

	if (priv->streaming || priv->tm_on)
		return -EBUSY;

	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret)
		return ret;

	seninf_write(priv, 0, SENINF_TOP_CTRL, SENINF_TOP_CTRL_TM);
	seninf_write(priv, 0, SENINF_CTRL, SENINF_TM_CTRL);
	seninf_write(priv, 0, SENINF_MUX_CTRL, SENINF_TM_MUX_CTRL);
	seninf_write(priv, 0, SENINF_MUX_INTEN, SENINF_TM_MUX_INTEN);
	seninf_write(priv, 0, SENINF_MUX_SPARE, 0);
	seninf_write(priv, 0, SENINF_MUX_CTRL_EXT, SENINF_TM_MUX_CTRL_EXT);
	seninf_write(priv, 0, SENINF_MUX_CTRL_EXT, 0);
	seninf_write(priv, 0, SENINF_TG_TM_CTL, SENINF_TM_TG_CTL);
	seninf_write(priv, 0, SENINF_TG_TM_SIZE, ((height + 0x100) << 16) | (width + 0x100));
	seninf_write(priv, 0, SENINF_TG_TM_CLK, 0);
	seninf_write(priv, 0, SENINF_TG_TM_STP, SENINF_TG_TM_STP_TEST);
	seninf_update(priv, 0, SENINF_CTRL_EXT, SENINF_CTRL_EXT_TM, SENINF_CTRL_EXT_TM);
	priv->tm_on = true;

	return 0;
}

static ssize_t mtk_seninf_dbg_tm_write(struct file *file, const char __user *ubuf,
				       size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
	unsigned int width, height;
	char buf[24];
	int ret = 0;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;

	mutex_lock(&priv->lock);
	if (sysfs_streq(buf, "off"))
		mtk_seninf_dbg_tm_stop(priv);
	else if (sscanf(buf, "%u %u", &width, &height) == 2 && width && height &&
		 width < 0x1000 && height < 0x1000)
		ret = mtk_seninf_dbg_tm_start(priv, width, height);
	else
		ret = -EINVAL;
	mutex_unlock(&priv->lock);

	return ret ?: len;
}

static ssize_t mtk_seninf_dbg_tm_read(struct file *file, char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct mtk_seninf *priv = file_inode(file)->i_private;
	char buf[4] = "";
	int n = scnprintf(buf, sizeof(buf), "%s\n", priv->tm_on ? "on" : "off");

	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations mtk_seninf_dbg_tm_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = mtk_seninf_dbg_tm_read,
	.write = mtk_seninf_dbg_tm_write,
};

void mtk_seninf_debugfs_init(struct mtk_seninf *priv)
{
	struct dentry *dir = debugfs_create_dir("mtk-seninf", NULL);

	priv->debugfs = dir;

	debugfs_create_file("status", 0444, dir, priv, &mtk_seninf_dbg_status_fops);
	debugfs_create_file("power", 0644, dir, priv, &mtk_seninf_dbg_power_fops);
	debugfs_create_file("regs", 0644, dir, priv, &mtk_seninf_dbg_regs_fops);
	debugfs_create_file("stream", 0644, dir, priv, &mtk_seninf_dbg_stream_fops);
	debugfs_create_file("reg", 0644, dir, priv, &mtk_seninf_dbg_reg_fops);
	debugfs_create_file("tm", 0644, dir, priv, &mtk_seninf_dbg_tm_fops);

	/* Applied by the next stream on */
	debugfs_create_x32("settle", 0644, dir, &priv->tune.settle);
	debugfs_create_x32("hs_trail", 0644, dir, &priv->tune.hs_trail);
	debugfs_create_u32("port", 0644, dir, &priv->tune.port);
	debugfs_create_u32("lanes", 0644, dir, &priv->tune.lanes);
	debugfs_create_x32("lane_map", 0644, dir, &priv->tune.lane_map);
}

void mtk_seninf_debugfs_exit(struct mtk_seninf *priv)
{
	debugfs_remove_recursive(priv->debugfs);

	mutex_lock(&priv->lock);
	mtk_seninf_dbg_tm_stop(priv);
	if (priv->dbg_power) {
		pm_runtime_put(priv->dev);
		priv->dbg_power = false;
	}
	mutex_unlock(&priv->lock);
}

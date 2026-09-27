// SPDX-License-Identifier: GPL-2.0
/*
 * Shutdown trace: write a line per device shutdown hook to a raw block device, synchronously, so a
 * reboot that never comes back leaves the name of the device whose hook did not return on disk.
 *
 * Enable with shutdown_trace=<block device path>@<byte offset>, for example
 * shutdown_trace=/dev/mmcblk0p42@32505856 (the last MiB of the Cosmo test boot slot). The log is
 * plain text: a header, "pre <device> <driver>" before each hook, "post <device>" after it, and
 * "done" when device_shutdown() finished. Read it back with dd + strings.
 *
 * The log cannot outlive its own storage: a write after the card or its host controller has been
 * shut down never completes. So the shutdown hooks of the block device's ancestors are skipped
 * ("skip <device>") while tracing; this is a debug aid, not something to leave enabled.
 */
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/shutdown_trace.h>

#define SHUTDOWN_TRACE_MAX	SZ_1M

static char st_spec[96];
static struct file *st_file;
static loff_t st_start, st_pos;
static bool st_failed;
static struct device *st_disk;

static int __init shutdown_trace_setup(char *str)
{
	strscpy(st_spec, str, sizeof(st_spec));
	return 1;
}
__setup("shutdown_trace=", shutdown_trace_setup);

static void st_write(const char *buf, size_t len)
{
	ssize_t n;

	if (!st_file || st_failed || st_pos - st_start + len > SHUTDOWN_TRACE_MAX)
		return;
	n = kernel_write(st_file, buf, len, &st_pos);
	if (n != len || vfs_fsync(st_file, 0)) {
		st_failed = true;
		pr_warn("shutdown_trace: write failed at %lld, stopping\n", st_pos);
	}
}

static void st_printf(const char *fmt, ...)
{
	char buf[160];
	va_list args;
	int len;

	va_start(args, fmt);
	len = vscnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	st_write(buf, len);
}

void shutdown_trace_begin(void)
{
	char *at, *path = st_spec;
	struct file *f;

	if (!st_spec[0])
		return;
	at = strchr(path, '@');
	if (!at || kstrtoll(at + 1, 0, &st_start)) {
		pr_warn("shutdown_trace: bad spec '%s' (want path@offset)\n", st_spec);
		return;
	}
	*at = '\0';
	f = filp_open(path, O_WRONLY | O_LARGEFILE, 0);
	if (IS_ERR(f)) {
		pr_warn("shutdown_trace: cannot open %s: %ld\n", path, PTR_ERR(f));
		return;
	}
	st_file = f;
	st_pos = st_start;
	if (S_ISBLK(file_inode(f)->i_mode))
		st_disk = disk_to_dev(file_bdev(f)->bd_disk);
	st_printf("shutdown_trace v1 uptime=%llu ms\n", ktime_get_boottime_ns() / NSEC_PER_MSEC);
	pr_info("shutdown_trace: logging device shutdown to %s@%lld\n", path, st_start);
}

static bool st_holds_log(struct device *dev)
{
	struct device *p;

	for (p = st_disk ? st_disk->parent : NULL; p; p = p->parent)
		if (p == dev)
			return true;
	return false;
}

bool shutdown_trace_pre(struct device *dev)
{
	if (!st_file)
		return false;
	if (st_holds_log(dev)) {
		st_printf("skip %s\n", dev_name(dev));
		return true;
	}
	st_printf("pre %s %s\n", dev_name(dev), dev->driver ? dev->driver->name : "-");
	return false;
}

void shutdown_trace_post(struct device *dev)
{
	if (!st_file)
		return;
	st_printf("post %s\n", dev_name(dev));
}

void shutdown_trace_end(void)
{
	if (!st_file)
		return;
	st_printf("done uptime=%llu ms\n", ktime_get_boottime_ns() / NSEC_PER_MSEC);
	/* keep the file open: closing it would itself touch a device that may already be gone */
}

/* The steps kernel_restart() takes after device_shutdown(), while block I/O still works. */
void shutdown_trace_mark(const char *what)
{
	if (!st_file)
		return;
	st_printf("mark %s cpu=%d uptime=%llu ms\n", what, raw_smp_processor_id(),
		  ktime_get_boottime_ns() / NSEC_PER_MSEC);
}

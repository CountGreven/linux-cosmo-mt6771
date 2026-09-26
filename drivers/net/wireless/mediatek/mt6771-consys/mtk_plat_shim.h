/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Stand-in for the vendor mt-plat headers the CONSYS stack includes.
 *
 * Every service that came from mt-plat/ in the 4.4 tree is either mapped to
 * a mainline API here or is a stub that logs and returns -ENOSYS. The map is
 * in projects/cosmo/17-consys-port.org. A stub is a STAND-IN for a real
 * implementation, marked as such below; none of them power the radio.
 *
 * Nothing here has run on the device: it compiles, which is all stage 1
 * promises.
 */
#ifndef MTK_PLAT_SHIM_H
#define MTK_PLAT_SHIM_H

#include <linux/compiler.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/types.h>

#define MTK_SHIM_STUB() \
	pr_warn_once("mt6771-consys: %s: not implemented on mainline (-ENOSYS)\n", __func__)

/*
 * Plain library calls the vendor code makes that 7.x removed. Exact
 * semantics are kept so the call sites do not change.
 */
static inline char *strncpy(char *dest, const char *src, size_t count)
{
	size_t i;

	for (i = 0; i < count && src[i]; i++)
		dest[i] = src[i];
	for (; i < count; i++)
		dest[i] = '\0';
	return dest;
}

/*
 * FB_EVENT_BLANK (linux/fb.h, removed): the fbdev screen-blank notification
 * that told the driver to enter or leave its screen-off power mode. Nothing
 * in mainline sends it any more, so the notifier the vendor registers never
 * fires.
 * STAND-IN: needs a panel/backlight follower (drm_panel_add_follower() or a
 * regulator/backlight event); the value is the vendor's.
 */
#define FB_EVENT_BLANK		0x09

/*
 * do_gettimeofday() and struct timeval are gone. The vendor keeps its
 * struct timeval variables (as struct __kernel_old_timeval, same two
 * fields) and reads the wall clock through this.
 */
static inline void mtk_gettimeofday(struct __kernel_old_timeval *tv)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / NSEC_PER_USEC;
}

/*
 * of_get_named_gpio() (linux/of_gpio.h, removed): the vendor reads global
 * GPIO numbers out of properties such as "gpio_combo_ldo_en_pin" and then
 * drives them with the legacy integer API.
 * STAND-IN: needs the properties renamed to "<name>-gpios" and read with
 * devm_gpiod_get(); that is a binding decision, not a compile fix.
 */
static inline int of_get_named_gpio(const struct device_node *np, const char *propname, int index)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

/*
 * mt-plat/sync_write.h -- a raw store followed by a full barrier. Copied,
 * not stubbed: it is arithmetic on an __iomem pointer, nothing platform.
 */
#define mt_reg_sync_writel(v, a) \
	do { \
		__raw_writel((v), (void __force __iomem *)(a)); \
		mb(); \
	} while (0)

#define mt_reg_sync_writew(v, a) \
	do { \
		__raw_writew((v), (void __force __iomem *)(a)); \
		mb(); \
	} while (0)

#define mt_reg_sync_writeb(v, a) \
	do { \
		__raw_writeb((v), (void __force __iomem *)(a)); \
		mb(); \
	} while (0)

/*
 * linux/wakelock.h (Android's, gone from mainline). Mapped to a wakeup
 * source: the same stay-awake / timed-wakeup / relax semantics.
 */
enum {
	WAKE_LOCK_SUSPEND,
	WAKE_LOCK_TYPE_COUNT
};

struct wake_lock {
	struct wakeup_source *ws;
};

static inline void wake_lock_init(struct wake_lock *lock, int type, const char *name)
{
	lock->ws = wakeup_source_register(NULL, name);
}

static inline void wake_lock_destroy(struct wake_lock *lock)
{
	wakeup_source_unregister(lock->ws);
	lock->ws = NULL;
}

static inline void wake_lock(struct wake_lock *lock)
{
	__pm_stay_awake(lock->ws);
}

static inline void wake_lock_timeout(struct wake_lock *lock, long timeout)
{
	__pm_wakeup_event(lock->ws, jiffies_to_msecs(timeout));
}

static inline void wake_unlock(struct wake_lock *lock)
{
	__pm_relax(lock->ws);
}

static inline int wake_lock_active(struct wake_lock *lock)
{
	return lock->ws && lock->ws->active;
}

/*
 * mt-plat/aee.h -- MediaTek's Android Exception Engine, a crash reporter
 * with no mainline counterpart. Reports become a kernel log line.
 * STAND-IN: could become a devcoredump for the CONSYS coredump path.
 */
#define DB_OPT_DEFAULT			0
#define DB_OPT_FTRACE			(1 << 0)
#define DB_OPT_WCN_ISSUE_INFO		(1 << 19)

static inline void aee_kernel_warning_api(const char *file, const int line, const int db_opt,
					  const char *module, const char *detail, ...)
{
	pr_warn("aee: %s:%d %s: %s\n", file, line, module, detail);
}

static inline void aed_combo_exception(const int *log, int log_size, const int *phy,
				       int phy_size, const char *detail)
{
	pr_warn("aee: combo exception: %s\n", detail);
}

static inline void aed_combo_exception_api(const int *log, int log_size, const int *phy,
					   int phy_size, const char *detail, const int db_opt)
{
	pr_warn("aee: combo exception: %s\n", detail);
}

#define aee_kernel_dal_show(msg)	pr_info("aee: %s", msg)
#define aee_kernel_exception(module, msg...)	pr_err("aee: %s: " msg)
#define aee_kernel_warning(module, msg...)	pr_warn("aee: %s: " msg)
#define aee_kernel_reminding(module, msg...)	pr_info("aee: %s: " msg)

/*
 * mt-plat/mtk_rtc.h -- the 32 kHz clock the PMIC RTC block drives to the
 * combo chip. mt6397-rtc does not expose it.
 * STAND-IN: needs the MT6358 XOSC32 output as a clock or a regulator-like
 * consumer; until then the radio has no sleep clock.
 */
enum rtc_gpio_user_t {
	RTC_GPIO_USER_WIFI = 8,
	RTC_GPIO_USER_GPS = 9,
	RTC_GPIO_USER_BT = 10,
	RTC_GPIO_USER_FM = 11,
	RTC_GPIO_USER_PMIC = 12,
};

static inline void rtc_gpio_enable_32k(enum rtc_gpio_user_t user)
{
	MTK_SHIM_STUB();
}

static inline void rtc_gpio_disable_32k(enum rtc_gpio_user_t user)
{
	MTK_SHIM_STUB();
}

/*
 * mtk_clkbuf_ctl.h -- the 26 MHz crystal buffer (XO_WCN) shared with the
 * PMIC. Nothing in mainline models it.
 * STAND-IN: needs a clk provider on the MT6358 clock buffer.
 */
enum clk_buf_id {
	CLK_BUF_CONN = 0,
};

static inline bool clk_buf_ctrl(enum clk_buf_id id, bool onoff)
{
	MTK_SHIM_STUB();
	return false;
}

static inline bool is_clk_buf_from_pmic(void)
{
	MTK_SHIM_STUB();
	return false;
}

/*
 * upmu_common.h -- register-level PMIC access (MT6358). Mainline has
 * regmap for the PMIC and the mt6358 regulator driver for vcn18, vcn28 and
 * vcn33; the HW0_OP_EN/CFG mode bits have no regulator-API equivalent.
 * STAND-IN: the enum values below are placeholders, not register numbers;
 * the real addresses are MT6358_PMIC_REG_BASE + 0x1c5a (VCN18 OP_EN) and
 * + 0x1d1e (VCN33 OP_EN) in the vendor's upmu_hw.h.
 */
enum mtk_shim_pmic_field {
	MT6358_LDO_VCN18_OP_EN = 0x1c5a,
	MT6358_LDO_VCN33_OP_EN = 0x1d1e,
	PMIC_RG_LDO_VCN28_HW0_OP_EN = 0x10000,
	PMIC_RG_LDO_VCN28_HW0_OP_CFG,
	PMIC_RG_LDO_VCN33_HW0_OP_EN,
	PMIC_RG_LDO_VCN33_HW0_OP_CFG,
};

static inline void pmic_config_interface(unsigned int reg, unsigned int val,
					 unsigned int mask, unsigned int shift)
{
	MTK_SHIM_STUB();
}

static inline void pmic_read_interface(unsigned int reg, unsigned int *val,
				       unsigned int mask, unsigned int shift)
{
	MTK_SHIM_STUB();
	*val = 0;
}

static inline void pmic_set_register_value(int flagname, unsigned int val)
{
	MTK_SHIM_STUB();
}

static inline unsigned short pmic_get_register_value(int flagname)
{
	MTK_SHIM_STUB();
	return 0;
}

static inline void upmu_set_reg_value(unsigned int reg, unsigned int reg_val)
{
	MTK_SHIM_STUB();
}

/*
 * mtk_sleep.h / mtk_spm_resource_req.h -- SPM wake-up reason. The AP's SPM
 * firmware is not driven from mainline.
 * STAND-IN: report "not woken by consys".
 */
typedef enum {
	WR_NONE = 0,
	WR_UART_BUSY = 1,
	WR_PCM_ASSERT = 2,
	WR_PCM_TIMER = 3,
	WR_WAKE_SRC = 4,
	WR_UNKNOWN = 5,
} wake_reason_t;

/* STAND-IN: the bit is a placeholder; the vendor value is in mtk_spm_internal.h */
#define WAKE_SRC_CONN2AP	(1U << 10)

static inline unsigned int slp_get_wake_reason(void)
{
	MTK_SHIM_STUB();
	return WR_NONE;
}

static inline unsigned int spm_get_last_wakeup_src(void)
{
	MTK_SHIM_STUB();
	return 0;
}

/*
 * mt_emi_api.h / mpu_v1.h -- EMI memory protection unit. The consys
 * reserved memory (0xbfc00000, 2 MiB) is fenced from every master but the
 * modem and consys. Mainline has no EMI MPU driver; today LK/TF-A leave it
 * as the boot chain set it.
 * STAND-IN: emi_mpu_set_protection() logs and reports failure.
 */
#define EMI_MPU_DOMAIN_NUM	16
#define EMI_MPU_DGROUP_NUM	(EMI_MPU_DOMAIN_NUM / 8)

#define NO_PROTECTION	0
#define SEC_RW		1
#define SEC_RW_NSEC_R	2
#define SEC_RW_NSEC_W	3
#define SEC_R_NSEC_R	4
#define FORBIDDEN	5
#define SEC_R_NSEC_RW	6

#define UNLOCK		0
#define LOCK		1

struct emi_region_info_t {
	unsigned long long start;
	unsigned long long end;
	unsigned int region;
	unsigned int apc[EMI_MPU_DGROUP_NUM];
};

#define SET_ACCESS_PERMISSION(apc_ary, lock, \
	d15, d14, d13, d12, d11, d10, d9, d8, d7, d6, d5, d4, d3, d2, d1, d0) \
do { \
	apc_ary[1] = \
		(((unsigned int)d15) << 21) | (((unsigned int)d14) << 18) | \
		(((unsigned int)d13) << 15) | (((unsigned int)d12) << 12) | \
		(((unsigned int)d11) << 9) | (((unsigned int)d10) << 6) | \
		(((unsigned int)d9) << 3) | ((unsigned int)d8); \
	apc_ary[0] = \
		(((unsigned int)d7) << 21) | (((unsigned int)d6) << 18) | \
		(((unsigned int)d5) << 15) | (((unsigned int)d4) << 12) | \
		(((unsigned int)d3) << 9) | (((unsigned int)d2) << 6) | \
		(((unsigned int)d1) << 3) | ((unsigned int)d0) | \
		((unsigned int)lock << 31); \
} while (0)

static inline int emi_mpu_set_protection(struct emi_region_info_t *region_info)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

/*
 * BTIF: the real driver now lives in btif/ (vendor drivers/misc/mediatek/btif adapted); the STP layer
 * calls it through the vendor's own exported header. No stubs.
 */
#include "btif/common/inc/mtk_btif_exp.h"

/* What the BTIF driver took from mach/mt_reg_base.h, mtk_io.h, mtk_lpae.h and mach/mt_irq.h: raw
 * register access with a completion barrier, the LPAE "4G mode" DMA flag (off: the rings come from
 * dma_alloc_coherent under a 32-bit mask), and the no-DT fallback addresses, which the DT path never
 * reads. */
#include <linux/io.h>
#ifndef mt_reg_sync_writel
#define mt_reg_sync_writel(v, a)	do { writel((v), (void __iomem *)(unsigned long)(a)); dsb(sy); } while (0)
#endif
#ifndef mt65xx_reg_sync_writel
#define mt65xx_reg_sync_writel(v, a)	mt_reg_sync_writel(v, a)
#endif
#ifndef enable_4G
#define enable_4G()			0
#endif
#ifndef AP_DMA_BASE
#define AP_DMA_BASE			0UL
#endif
#ifndef BTIF_BASE
#define BTIF_BASE			0UL
#endif
#ifndef MT_BTIF_IRQ_ID
#define MT_BTIF_IRQ_ID			0
#endif
/* mtk-gic-extend.h: a GIC register dump for debugging; nothing to dump on the mainline GIC driver. */
#ifndef mt_irq_dump_status
#define mt_irq_dump_status(irq)		do { (void)(irq); } while (0)
#endif




#endif /* MTK_PLAT_SHIM_H */

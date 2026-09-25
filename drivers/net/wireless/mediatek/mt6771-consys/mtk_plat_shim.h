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
 * mtk_btif_exp.h -- the BTIF UART-like block at 0x1100c000 that carries
 * STP to the combo chip. It is a MediaTek IP with its own vendor driver
 * (drivers/misc/mediatek/btif); mainline has none.
 * STAND-IN: every call fails with -ENOSYS, so stp_btif cannot open.
 */
#define BTIF_MAX_LEN_PER_PKT	2048

typedef enum _ENUM_BTIF_DPIDLE_ {
	BTIF_DPIDLE_DISABLE = 0,
	BTIF_DPIDLE_ENABLE,
	BTIF_DPIDLE_MAX,
} ENUM_BTIF_DPIDLE_CTRL;

typedef enum _ENUM_BTIF_LPBK_MODE_ {
	BTIF_LPBK_DISABLE = 0,
	BTIF_LPBK_ENABLE,
	BTIF_LPBK_MAX,
} ENUM_BTIF_LPBK_MODE;

typedef enum _ENUM_BTIF_DBG_ID_ {
	BTIF_DISABLE_LOGGER = 0,
	BTIF_ENABLE_LOGGER,
	BTIF_DUMP_LOG,
	BTIF_CLR_LOG,
	BTIF_DUMP_BTIF_REG,
	BTIF_ENABLE_RT_LOG,
	BTIF_DISABLE_RT_LOG,
	BTIF_DUMP_BTIF_IRQ,
	BTIF_DBG_MAX,
} ENUM_BTIF_DBG_ID;

typedef int (*MTK_WCN_BTIF_RX_CB)(const unsigned char *p_buf, unsigned int len);

struct task_struct;

static inline int mtk_wcn_btif_open(char *p_owner, unsigned long *p_id)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_close(unsigned long u_id)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_write(unsigned long u_id, const unsigned char *p_buf, unsigned int len)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_read(unsigned long u_id, unsigned char *p_buf, unsigned int max_len)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_dpidle_ctrl(unsigned long u_id, ENUM_BTIF_DPIDLE_CTRL en_flag)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_rx_cb_register(unsigned long u_id, MTK_WCN_BTIF_RX_CB rx_cb)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_wakeup_consys(unsigned long u_id)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_loopback_ctrl(unsigned long u_id, ENUM_BTIF_LPBK_MODE enable)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline int mtk_wcn_btif_dbg_ctrl(unsigned long u_id, ENUM_BTIF_DBG_ID flag)
{
	MTK_SHIM_STUB();
	return -ENOSYS;
}

static inline bool mtk_wcn_btif_parser_wmt_evt(unsigned long u_id, const char *sub_str,
					       unsigned int str_len)
{
	MTK_SHIM_STUB();
	return false;
}

static inline int mtk_btif_exp_rx_has_pending_data(unsigned long u_id)
{
	MTK_SHIM_STUB();
	return 0;
}

static inline int mtk_btif_exp_tx_has_pending_data(unsigned long u_id)
{
	MTK_SHIM_STUB();
	return 0;
}

static inline struct task_struct *mtk_btif_exp_rx_thread_get(unsigned long u_id)
{
	MTK_SHIM_STUB();
	return NULL;
}

#endif /* MTK_PLAT_SHIM_H */

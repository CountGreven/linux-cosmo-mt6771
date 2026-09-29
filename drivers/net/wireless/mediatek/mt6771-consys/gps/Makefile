# SPDX-License-Identifier: GPL-2.0-only
# GPS function of the CONSYS radio: /dev/stpgps on the STP transport in wmt_drv, and /dev/gps_emi.
# The flags are repeated here so that the directory also builds alone (make M=).
C := $(srctree)/drivers/net/wireless/mediatek/mt6771-consys
ccflags-y += -I$(C) -I$(C)/common
ccflags-y += -I$(C)/common/common_main/include -I$(C)/common/common_main/linux/include
ccflags-y += -I$(C)/common/common_main/core/include -I$(C)/common/common_main/platform/include
ccflags-y += -include $(C)/mtk_plat_shim.h
ccflags-y += -DCONFIG_ANDROID=1 -DCONFIG_WAKELOCK=1
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT -DWMT_IDC_SUPPORT=0
ccflags-y += -DMTK_WCN_REMOVE_KERNEL_MODULE -DCONFIG_MTK_WCN_ARM64
ccflags-y += -DSOC_CO_CLOCK_FLAG=1 -DWMT_CREATE_NODE_DYNAMIC=1 -DREMOVE_MK_NODE=0
ccflags-y += -DEMI_MPU_PROTECTION_IS_READY=1

obj-$(CONFIG_MT6771_CONSYS_GPS) += gps_drv.o
gps_drv-y := stp_chrdev_gps.o gps_emi.o

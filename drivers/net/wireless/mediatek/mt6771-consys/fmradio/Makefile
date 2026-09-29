# SPDX-License-Identifier: GPL-2.0-only
# FM function of the CONSYS radio (an MT6631 FM core): /dev/fm on the STP transport in wmt_drv.
# The flags are repeated here so that the directory also builds alone (make M=).
C := $(srctree)/drivers/net/wireless/mediatek/mt6771-consys
ccflags-y += -I$(src)/inc -I$(src)/mt6631/inc
ccflags-y += -I$(C) -I$(C)/common
ccflags-y += -I$(C)/common/common_main/include -I$(C)/common/common_main/linux/include
ccflags-y += -include $(C)/mtk_plat_shim.h
ccflags-y += -DCONFIG_ANDROID=1 -DCONFIG_WAKELOCK=1
ccflags-y += -DCONFIG_MTK_WCN_ARM64 -DWMT_IDC_SUPPORT=0
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT -DMT6631_FM

obj-$(CONFIG_MT6771_CONSYS_FM) += fmradio_drv.o
fmradio_drv-y := core/fm_module.o core/fm_main.o core/fm_config.o core/fm_rds_parser.o \
		 core/fm_patch.o core/fm_utils.o core/fm_link.o core/fm_eint.o core/fm_cmd.o \
		 mt6631/pub/mt6631_fm_lib.o mt6631/pub/mt6631_fm_rds.o

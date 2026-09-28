# SPDX-License-Identifier: GPL-2.0-only
# Bluetooth function of the CONSYS radio as a kernel HCI device, on the STP transport in wmt_drv.
# The flags are repeated here so that the directory also builds alone (make M=).
C := $(srctree)/drivers/net/wireless/mediatek/mt6771-consys
ccflags-y += -I$(C) -I$(C)/common
ccflags-y += -I$(C)/common/common_main/include -I$(C)/common/common_main/linux/include
ccflags-y += -include $(C)/mtk_plat_shim.h
ccflags-y += -DCONFIG_ANDROID=1 -DCONFIG_WAKELOCK=1
ccflags-y += -DMTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT -DWMT_IDC_SUPPORT=0

obj-$(CONFIG_MT6771_CONSYS_BT) += hci_stp.o

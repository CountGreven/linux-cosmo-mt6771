# SPDX-License-Identifier: GPL-2.0-only
#
# MT6771 CONSYS wifi: vendor 4.4 sources in common/ and wlan/, built as they
# were. The vendor top-level Makefile did this with obj-y "connadp" (the
# build-in adapters) plus the three subdirectories below.

# This directory carries 4.4-era vendor code: it has to compile, not be clean.
subdir-ccflags-y += -Wno-error

# The vendor sources include mt-plat headers and test CONFIG_ANDROID /
# CONFIG_WAKELOCK; the shim stands in for the first and these flags for the
# second (mainline has neither symbol). The shim is force-included so the
# 4.4 -> 7.x compatibility it carries reaches every object.
subdir-ccflags-y += -I$(src) -I$(src)/common
subdir-ccflags-y += -I$(srctree)/drivers/mmc/core
subdir-ccflags-y += -include $(src)/mtk_plat_shim.h
subdir-ccflags-y += -DCONFIG_ANDROID=1 -DCONFIG_WAKELOCK=1

connadp-y := common/connectivity_build_in_adapter.o common/wmt_build_in_adapter.o
obj-$(CONFIG_MT6771_CONSYS) += connadp.o
obj-$(CONFIG_MT6771_CONSYS) += common/
obj-$(CONFIG_MT6771_CONSYS) += wlan/adaptor/
obj-$(CONFIG_MT6771_CONSYS) += wlan/core/gen3/

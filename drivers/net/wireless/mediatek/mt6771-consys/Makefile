# SPDX-License-Identifier: GPL-2.0-only
#
# MT6771 CONSYS wifi: vendor 4.4 sources in common/ and wlan/, built as they
# were. The vendor top-level Makefile did this with obj-y "connadp" (the
# build-in adapters) plus the three subdirectories below.

# This directory carries 4.4-era vendor code: it has to compile, not be clean.
subdir-ccflags-y += -Wno-error

connadp-y := common/connectivity_build_in_adapter.o common/wmt_build_in_adapter.o
obj-$(CONFIG_MT6771_CONSYS) += connadp.o
obj-$(CONFIG_MT6771_CONSYS) += common/
obj-$(CONFIG_MT6771_CONSYS) += wlan/adaptor/
obj-$(CONFIG_MT6771_CONSYS) += wlan/core/gen3/

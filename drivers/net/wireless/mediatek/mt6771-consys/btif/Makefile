# BTIF: the UART-like link between the AP and the connectivity MCU; the WMT/STP transport rides on it.
# Vendor driver (drivers/misc/mediatek/btif, 4.4 "Initial import" 06c6c5595) adapted to mainline APIs
# through the port's shim. The DMA engine is driven directly, as the vendor does. One module, loaded
# before wmt_drv, which imports its exported entry points.
subdir-ccflags-y += -DBTIF_DBG_SUPPORT=0 -I$(src)/common/inc -I$(src)/common/plat_inc
obj-$(CONFIG_MT6771_CONSYS) += btif_drv.o
btif_drv-y := common/mtk_btif.o common/mtk_btif_exp.o common/btif_plat.o common/btif_dma_plat.o

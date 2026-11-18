# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(srctree)/drivers/iommu
ccflags-y += -I$(srctree)/drivers/misc/mediatek/mtk-interconnect
obj-$(CONFIG_INTERCONNECT_MTK_EMI) += mtk-emi.o
obj-$(CONFIG_INTERCONNECT_MTK_MMQOS_COMMON) += mmqos-common.o
mmqos-common-objs += mmqos-mtk.o mmqos-hrt.o
obj-$(CONFIG_INTERCONNECT_MTK_MMQOS_MT8169) += mmqos-mt8169.o

# SPDX-License-Identifier: GPL-2.0
dtb-$(CONFIG_ARCH_MEDIATEK) += mt2712-evb.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt6755-evb.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt6795-evb.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt6797-evb.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt6797-x20-dev.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt7622-rfb1.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt7622-bananapi-bpi-r64.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt8169.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt8169-raspite.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt8173-evb.dtb
dtb-$(CONFIG_ARCH_MEDIATEK) += mt8183-evb.dtb

dtbo-$(CONFIG_ARCH_MEDIATEK) += raspite_proto.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += raspite_proto_wpc.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += raspite_hvt.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += raspite_hvt_wpc.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += raspite_evt.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += raspite_evt_wpc.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += tungsten_proto.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += tungsten_hvt.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += tungsten_evt.dtbo
dtbo-$(CONFIG_ARCH_MEDIATEK) += tungsten_dvt.dtbo

DTC_FLAGS += -@

extra-y += $(dtbo-y)

$(obj)/%.dtbo: $(src)/%.dts $(DTC) FORCE
	$(call if_changed_dep,dtc,dtb)

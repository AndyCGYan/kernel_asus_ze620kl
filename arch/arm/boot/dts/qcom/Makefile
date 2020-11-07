ifeq ($(ASUS_BUILD_PROJECT),ZE620KL)
dtb-$(CONFIG_ARCH_SDM660) += \
	sdm660-cdp.dtb \
	sdm660-mtp.dtb \
	sdm660-qrd.dtb \
	ara-er-sdm636-qrd.dtb \
	ara-er-oc-sdm636-qrd.dtb

else ifeq ($(ASUS_BUILD_PROJECT),ZE554KL)
dtb-$(CONFIG_ARCH_SDM660) += \
	sdm660-cdp.dtb \
	sdm660-mtp.dtb \
	ze554kl-er-sdm660-qrd.dtb

dtb-$(CONFIG_ARCH_SDM630) +=  \
	sdm630-mtp.dtb \
	ze554kl-er-sdm630-cdp.dtb \
	sdm630-pm660a-qrd.dtb

else ifeq ($(ASUS_BUILD_PROJECT),ZC600KL)

dtb-$(CONFIG_ARCH_SDM630) +=  \
	zc600kl-sdm630-pm660l-qrd.dtb

endif #end of ifeq ($(ASUS_BUILD_PROJECT),ZE620KL)

ifeq ($(CONFIG_ARM64),y)
always          := $(dtb-y)
always          += $(dtbo-y)
subdir-y        := $(dts-dirs)
else
targets += dtbs
targets += $(addprefix ../, $(dtb-y))

$(obj)/../%.dtb: $(src)/%.dts FORCE
	$(call if_changed_dep,dtc)

dtbs: $(addprefix $(obj)/../,$(dtb-y))
endif
clean-files := *.dtbo *.dtb

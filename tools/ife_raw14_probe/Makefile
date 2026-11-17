ifneq ($(KERNELRELEASE),)
obj-m += ife_raw14_probe.o
ccflags-y += -D__LINUX
ccflags-y += -I$(src)/generated
ccflags-y += -I$(SDK_CODE)/hdal/drivers/k_driver/include
ccflags-y += -I$(SDK_CODE)/vos/drivers/include
else
.PHONY: all prepare clean
all: prepare
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) SDK_CODE=$(SDK_CODE) modules
prepare:
	python3 prepare_sdk.py --sdk "$(SDK_CODE)"
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
endif

MODULE_NAME = Tiffany_thermal
# DIRs
VOS_DRIVER_DIR = $(NVT_VOS_DIR)/drivers
HDAL_SAMPLE_DIR = $(NVT_HDAL_DIR)/samples
OUTPUT_DIR = $(HDAL_SAMPLE_DIR)/output
# INCs
VOS_INC_PATH = $(VOS_DRIVER_DIR)/include
HDAL_INC_PATH = $(NVT_HDAL_DIR)/include
HDAL_LIB_PATH = $(NVT_HDAL_DIR)/output
VOS_LIB_PATH = $(NVT_VOS_DIR)/output
# INC FLAGs
EXTRA_INCLUDE += \
	-I$(HDAL_INC_PATH) \
	-I$(VOS_INC_PATH) \
	-I$(NVT_VOS_DIR)/include \
	-I$(LIBRARY_DIR)/include \
	-I$(NVT_DRIVER_DIR)/include \
	-I$(NVT_HDAL_DIR)/vendor/media/include \
	-I$(NVT_HDAL_DIR)/vendor/isp/include \

###############################################################################
# Linux Makefile                                                              #
###############################################################################
ifeq ($(NVT_PRJCFG_CFG),Linux)
EXTRA_CFLAGS += $(NVT_ASAN)
EXTRA_LDFLAGS = \
	$(NVT_ASAN) \
	-L$(HDAL_LIB_PATH) -lhdal -lm -lpthread \
	-L$(NVT_HDAL_DIR)/vendor/output -lvendor_media -lvendor_isp \
	-L$(NVT_HDAL_DIR)/vendor/output -lprebuilt_isp \
	-L$(VOS_LIB_PATH) -lvos \
	-L$(LIBRARY_DIR)/output \
	-L$(NVT_DRIVER_DIR)/output \
	-Wl,--start-group \
	-l:libuvac.a -l:libuvcp.a \
	-l:libusb2dev.a -l:libusb3dev.a \
	-l:libutility.a -l:libSxTimer.a -l:libfwsrv.a \
	-l:libfdt.a -l:libzlib.a -l:libstrg.a \
	-l:libmsdcnvt.a -l:libumsd.a \
	-l:libFileSys.a -l:libFsLinux.a \
	-Wl,--end-group -lrt -lstdc++
SRC += \
	main.c \
	app.c \
	Pipeline/menu.c \
	Config/app_config.c \
	Config/hardware_config.c \
	Pipeline/liveview.c \
	Pipeline/Output/video_output.c \
	Pipeline/UVC/uvc_output.c \
	Pipeline/RTSP/rtsp_output.c \
	Algorithm/BPR/bpr.c \
	Algorithm/NUC/nuc.c \
	Algorithm/FFC/ffc.c \
	Algorithm/TDC/tdc.c \
	Enhancement/Gamma/gamma.c \
	Driver/GPIO/gpio_control.c \
	Driver/SPI/spi.c \
	Driver/I2C/i2c.c \
	Device/sensor_temp.c

###############################################################################
# rtos Makefile                                                               #
###############################################################################
else ifeq ($(NVT_PRJCFG_CFG),rtos)
# DIRs
RTOS_KERNEL_DIR = $(KERNELDIR)/lib/FreeRTOS
RTOS_LIB_DIR = $(KERNELDIR)/lib
RTOS_CURR_DEMO_DIR = $(KERNELDIR)/demos/novatek/$(SDK_CODENAME)
RTOS_POSIX_DIR = $(KERNELDIR)/lib/FreeRTOS-Plus-POSIX
RTOS_POSIX_SRC_DIR = $(RTOS_POSIX_DIR)/source

#INCs for EXTRA_CFLAGS

EXTRA_INCLUDE += \
	-I$(RTOS_LIB_DIR) \
	-I$(RTOS_KERNEL_DIR)/portable/GCC/ARM_CA9  \
	-I$(RTOS_KERNEL_DIR)/include \
	-I$(RTOS_KERNEL_DIR)/include/private \
	-I$(RTOS_CURR_DEMO_DIR)/include \
	-I$(RTOS_POSIX_DIR)/include \
	-I$(RTOS_POSIX_DIR)/include/portable/novatek \
	-I$(LIBRARY_DIR)/include

EXTRA_CFLAGS += -Wno-format

endif

include $(NVT_HDAL_DIR)/executable.mk

include ../Makefile.defs
TARGETS := recovery
$(OUTPUT_DIRECTORY)/recovery.out: LINKER_SCRIPT := recovery.ld
APP := ../application/src
TINYUSB := ../../.cache/web/deps/tinyusb
LWIP := ../../.cache/web/deps/lwip
WEB_BUILD := ../../.cache/web/recovery

SRC_FILES += $(wildcard $(PROJ_DIR)/*.c) $(SRC_COMMON)/web_update.c $(SRC_COMMON)/nordic_update.c \
  $(SRC_COMMON)/hw_connect.c $(SRC_COMMON)/libc_nano_stubs.c \
  ../bootloader/src/dfu_public_key.c $(APP)/usb_main.c \
  $(APP)/net/http.c $(APP)/net/network.c $(APP)/net/dhcp.c $(APP)/net/rndis.c $(APP)/net/bytes.c \
  $(SDK_ROOT)/modules/nrfx/mdk/system_nrf52840.c \
  $(PROJ_DIR)/startup.S \
  $(SDK_ROOT)/modules/nrfx/hal/nrf_nvmc.c \
  $(TINYUSB)/src/tusb.c $(TINYUSB)/src/common/tusb_fifo.c \
  $(TINYUSB)/src/device/usbd.c $(TINYUSB)/src/device/usbd_control.c \
  $(TINYUSB)/src/class/cdc/cdc_device.c $(TINYUSB)/src/class/net/ecm_rndis_device.c \
  $(TINYUSB)/src/portable/nordic/nrf5x/dcd_nrf5x.c \
  $(wildcard $(LWIP)/src/core/*.c) \
  $(filter-out $(LWIP)/src/core/ipv4/dhcp.c,$(wildcard $(LWIP)/src/core/ipv4/*.c)) \
  $(LWIP)/src/netif/ethernet.c
INC_FOLDERS += $(PROJ_DIR) $(SRC_COMMON) $(APP) $(APP)/net $(WEB_BUILD) \
  $(TINYUSB)/src $(TINYUSB)/lib/networking $(LWIP)/src/include \
  $(SDK_ROOT)/modules/nrfx $(SDK_ROOT)/modules/nrfx/mdk $(SDK_ROOT)/modules/nrfx/hal \
  $(SDK_ROOT)/modules/nrfx/drivers/include $(SDK_ROOT)/modules/nrfx/drivers/src $(SDK_ROOT)/integration/nrfx \
  $(SDK_ROOT)/components/toolchain/cmsis/include $(SDK_ROOT)/components/libraries/util \
  $(SDK_ROOT)/components/libraries/delay \
  $(SDK_ROOT)/components/softdevice/s140/headers $(SDK_ROOT)/components/softdevice/s140/headers/nrf52 \
  $(SDK_ROOT)/external/nrf_oberon/include

OPT := -Os -g3
CFLAGS += $(OPT) -DWEB_RECOVERY -DPROJECT_CHAMELEON_ULTRA -DNRF52840_XXAA \
  -DAPP_FW_VER_MAJOR=0 -DAPP_FW_VER_MINOR=2 \
  -D__HEAP_SIZE=0 -D__STACK_SIZE=8192 -DFLOAT_ABI_HARD \
  -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -Wall -Werror -ffunction-sections -fdata-sections -fno-strict-aliasing -fshort-enums
ASMFLAGS += -g3 -DNRF52840_XXAA -D__HEAP_SIZE=0 -D__STACK_SIZE=8192 -DFLOAT_ABI_HARD \
  -mcpu=cortex-m4 -mthumb -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16
LDFLAGS += $(OPT) -mthumb -mcpu=cortex-m4 -mabi=aapcs -mfloat-abi=hard -mfpu=fpv4-sp-d16 \
  -L$(SDK_ROOT)/modules/nrfx/mdk -T$(LINKER_SCRIPT) -Wl,--gc-sections --specs=nano.specs
LIB_FILES += $(SDK_ROOT)/external/nrf_oberon/lib/cortex-m4/hard-float/liboberon_3.0.8.a -lc -lnosys -lm
.PHONY: default
default: recovery
TEMPLATE_PATH := $(SDK_ROOT)/components/toolchain/gcc
include $(TEMPLATE_PATH)/Makefile.common
$(foreach target, $(TARGETS), $(call define_target, $(target)))

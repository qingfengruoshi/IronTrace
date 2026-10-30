# ============================================================================
# Makefile �?PD245 V3 重写固件（STM32L431CCT6，裸机无 HAL�?# 需�?arm-none-eabi-gcc（ARM GNU Toolchain�?# ============================================================================
TARGET  = pd245_fw
BUILD   = build
CROSS   = arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

CFLAGS  = -mcpu=cortex-m4 -mthumb -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin
CFLAGS += -ffunction-sections -fdata-sections -fno-common
# �?-ffreestanding 必须保留: 否则 -O2 会把 mini_lib.c 里手写的 memset/memcpy 循环
#   替换�?调用 memset 自身"(循环模式识别), 产生无限递归、栈溢出跑飞 (2026-10-06 实锤)
CFLAGS += -ffreestanding
LDFLAGS = -mcpu=cortex-m4 -mthumb -T stm32l431.ld -nostdlib
LDFLAGS+= -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

SRCS = startup.c main.c lcd.c hw.c mini_lib.c flash.c
OBJS = $(addprefix $(BUILD)/,$(SRCS:.c=.o))

all: $(BUILD)/$(TARGET).bin
	@$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/%.o: %.c regs.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) stm32l431.ld
	$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

flash: $(BUILD)/$(TARGET).bin
	@echo "�?Power Writer / pyOCD 烧录 $(BUILD)/$(TARGET).bin �?0x08000000"

clean:
	rm -rf $(BUILD)

.PHONY: all flash clean

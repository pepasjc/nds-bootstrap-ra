#---------------------------------------------------------------------------------
# RetroAchievements ARM11 program for TWPatch's TwlBg (3DS), uploaded by the
# card engine over RTCom.  Output: ../arm7/source/ra_uc11.h (the bytes).
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
include $(DEVKITARM)/base_tools

BUILD	:=	build
OUT	:=	../arm7/source/ra_uc11.h
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=soft -marm
CFLAGS	:=	-Os -Wall -ffreestanding -fno-builtin -fomit-frame-pointer -ffunction-sections \
		-fno-tree-loop-distribute-patterns -mword-relocations $(ARCH)
LDFLAGS	:=	-nostartfiles -nostdlib -T ucode.ld -Wl,--gc-sections -Wl,-Map,$(BUILD)/ra_uc11.map $(ARCH)

OFILES	:=	$(BUILD)/entry.o $(BUILD)/main.o $(BUILD)/mcu.o

all: $(OUT)

$(OUT): $(BUILD)/ra_uc11.bin
	@echo "// Generated from cardenginei/arm11_ra (make there): the ARM11 program" > $@
	@echo "static const unsigned char raUc11[] = {" >> $@
	@od -An -v -tx1 $< | sed 's/ *\([0-9a-f][0-9a-f]\)/0x\1,/g' >> $@
	@echo "};" >> $@
	@echo built ... ra_uc11.h $$(stat -c %s $<) bytes

$(BUILD)/ra_uc11.bin: $(BUILD)/ra_uc11.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/ra_uc11.elf: $(OFILES) ucode.ld
	$(CC) $(LDFLAGS) $(OFILES) -lgcc -o $@

$(BUILD)/%.o: source/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: source/%.s | $(BUILD)
	$(CC) $(ARCH) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

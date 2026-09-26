#---------------------------------------------------------------------------------
# RetroAchievements engine: rcheevos + ra_engine.c, linked to run from RA_REGION
# on the ARM7.  Output: nitrofiles/cardenginei_arm7_ra.bin (not compressed; the
# loader copies it into ramDump.bin for the card engine to load).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

RCHEEVOS	:=	$(CURDIR)/../../../external/rcheevos
BUILD		:=	build
TARGET		:=	cardenginei_arm7_ra
OUTPUT		:=	$(CURDIR)/../../nitrofiles/$(TARGET).bin

RC_SOURCES	:=	alloc.c condition.c condset.c memref.c operand.c trigger.c value.c
SOURCES		:=	$(wildcard source/*.c) $(wildcard source/*.s) \
				$(addprefix $(RCHEEVOS)/src/rcheevos/,$(RC_SOURCES)) 				$(RCHEEVOS)/src/rc_util.c

INCLUDE		:=	-I$(CURDIR)/../../common/include -I$(LIBNDS)/include \
				-I$(RCHEEVOS)/include -I$(RCHEEVOS)/src

ARCH		:=	-mthumb -mthumb-interwork -march=armv4t -mcpu=arm7tdmi -mtune=arm7tdmi

CFLAGS		:=	-Wall -Os -fomit-frame-pointer -ffunction-sections -fdata-sections \
				-fno-strict-aliasing $(ARCH) $(INCLUDE) -DARM7 \
				-DRC_DISABLE_LUA -DRC_NO_THREADS -DNDEBUG
ASFLAGS		:=	-mthumb-interwork -march=armv4t $(INCLUDE)
LDFLAGS		:=	-nostartfiles -T $(BUILD)/cardengine.ld $(ARCH) -Wl,--gc-sections \
				-Wl,--nmagic -Wl,-Map,$(BUILD)/$(TARGET).map

OBJECTS		:=	$(addprefix $(BUILD)/,$(addsuffix .o,$(notdir $(basename $(SOURCES)))))

vpath %.c source $(RCHEEVOS)/src/rcheevos $(RCHEEVOS)/src
vpath %.s source

.PHONY: all clean

all: $(OUTPUT)

$(BUILD):
	@mkdir -p $@

$(BUILD)/cardengine.ld: cardengine.ld.in | $(BUILD)
	$(CC) -E -P -x c -DRA_LINKER_SCRIPT $(INCLUDE) $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) $(BUILD)/cardengine.ld
	$(CC) $(LDFLAGS) $(OBJECTS) -lm -o $@

$(OUTPUT): $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo built ... $(notdir $@) $$(stat -c %s $@) bytes

clean:
	@rm -rf $(BUILD) $(OUTPUT)

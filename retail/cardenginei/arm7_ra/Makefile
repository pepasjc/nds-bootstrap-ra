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

RC_SOURCES	:=	alloc.c condition.c condset.c format.c lboard.c memref.c operand.c richpresence.c runtime.c value.c
SOURCES		:=	$(wildcard source/*.c) $(wildcard source/*.s) \
				$(addprefix $(RCHEEVOS)/src/rcheevos/,$(RC_SOURCES)) \
				$(RCHEEVOS)/src/rc_util.c $(RCHEEVOS)/src/rc_compat.c $(RCHEEVOS)/src/rhash/md5.c \
				$(BUILD)/trigger_sc.c

INCLUDE		:=	-I$(CURDIR)/../../common/include -I$(LIBNDS)/include \
				-I$(RCHEEVOS)/include -I$(RCHEEVOS)/src -I$(RCHEEVOS)/src/rcheevos

ARCH		:=	-mthumb -mthumb-interwork -march=armv4t -mcpu=arm7tdmi -mtune=arm7tdmi

CFLAGS		:=	-Wall -O2 -fomit-frame-pointer -ffunction-sections -fdata-sections \
				-fno-strict-aliasing $(ARCH) $(INCLUDE) -DARM7 \
				-DRC_DISABLE_LUA -DRC_NO_THREADS -DNDEBUG
ASFLAGS		:=	-mthumb-interwork -march=armv4t $(INCLUDE)
LDFLAGS		:=	-nostartfiles -T $(BUILD)/cardengine.ld $(ARCH) -Wl,--gc-sections \
				-Wl,--nmagic -Wl,-Map,$(BUILD)/$(TARGET).map

OBJECTS		:=	$(addprefix $(BUILD)/,$(addsuffix .o,$(notdir $(basename $(SOURCES)))))

vpath %.c source $(RCHEEVOS)/src/rcheevos $(RCHEEVOS)/src $(RCHEEVOS)/src/rhash
vpath %.s source

.PHONY: all clean

all: $(OUTPUT)

$(BUILD):
	@mkdir -p $@

$(BUILD)/cardengine.ld: cardengine.ld.in $(CURDIR)/../../common/include/ra_engine.h | $(BUILD)
	$(CC) -E -P -x c -DRA_LINKER_SCRIPT $(INCLUDE) $< -o $@

# rcheevos never lets a trigger stop at its first false condition.  That is
# safe to allow: hit counts, pauses and measured values are evaluated in
# passes that never short-circuit, so it only skips conditions without side
# effects, and it halves the work per frame (tools/ra_bench.sh).
$(BUILD)/trigger_sc.c: $(RCHEEVOS)/src/rcheevos/trigger.c | $(BUILD)
	sed 's/memset(&eval_state, 0, sizeof(eval_state));/& eval_state.can_short_curcuit = 1;/' $< > $@
	grep -q 'eval_state.can_short_curcuit = 1;' $@

$(BUILD)/trigger_sc.o: $(BUILD)/trigger_sc.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	$(CC) -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) $(BUILD)/cardengine.ld
	$(CC) $(LDFLAGS) $(OBJECTS) -lm -o $@

$(OUTPUT): $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@
	@echo built ... $(notdir $@) $$(stat -c %s $@) bytes

clean:
	@rm -rf $(BUILD) $(OUTPUT)

-include $(OBJECTS:.o=.d)

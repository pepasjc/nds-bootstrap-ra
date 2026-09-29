// RetroAchievements on a 3DS: a small ARM11 program the card engine uploads
// into TWPatch's TwlBg over RTCom (the RTC registers both CPUs see).  Each
// call gets one byte from the ARM7 and answers one byte.
//
// Commands (ra_ucode.h):
//   RA_UC_HELLO  -> RA_UC_MAGIC (so a program still loaded from the last
//                   game isn't uploaded again)
//   RA_UC_LED    -> 0 once the notification LED plays the unlock pattern
//   RA_UC_LED_OFF-> 0 once it's off
// Errors: 0x80 | step for MCU lookups that failed, 0x40 for a failed write.
#include <stdint.h>
#include <stdbool.h>
#include "../../arm7/source/ra_ucode.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

int mcuFind(void);
bool mcuWrite(int reg, const u8* data, int len);

// The notification LED (MCU register 0x2D): speed, smoothing, loop delay,
// then 32 steps of red, green and blue
struct LedPattern {
	u8 delay;
	u8 smoothing;
	u8 loopDelay;  // 0xFF: once
	u8 unused;
	u8 r[32], g[32], b[32];
};

static int s_mcu = -1; // mcuFind() result, 0 = found

static int led(bool on)
{
	if (s_mcu < 0) {
		s_mcu = mcuFind();
	}
	if (s_mcu != 0) {
		return 0x80 | s_mcu;
	}
	static struct LedPattern p;
	u8* raw = (u8*)&p;
	for (unsigned i = 0; i < sizeof(p); i++) {
		raw[i] = 0;
	}
	if (on) {
		// Three gold flashes, like the achievement popup's chime
		p.delay = 0x10;
		p.smoothing = 0x08;
		p.loopDelay = 0xFF;
		for (int i = 0; i < 12; i++) {
			const bool lit = (i % 4) < 2;
			p.r[i] = lit ? 0xFF : 0;
			p.g[i] = lit ? 0xA0 : 0;
		}
	}
	return mcuWrite(0x2D, raw, sizeof(p)) ? 0 : 0x40;
}

int raUcodeCommand(u8 command, u32 unused)
{
	(void)unused;
	switch (command) {
		case RA_UC_HELLO: return RA_UC_MAGIC;
		case RA_UC_LED: return led(true);
		case RA_UC_LED_OFF: return led(false);
	}
	return 0xEE;
}

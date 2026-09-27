#include <nds/ndstypes.h>
#include <nds/ipc.h>
#include <nds/interrupts.h>
#include <nds/input.h>
#include <nds/arm7/audio.h>
#include <nds/arm7/clock.h>
#include <nds/arm7/i2c.h>
#include <time.h>

#include "igm_text.h"
#include "ra_popup.h"
#include "ra_engine.h"
#include "locations.h"
#include "cardengine.h"
#include "fpsAdjust.h"
#include "nds_header.h"
#include "tonccpy.h"

#define sleepMode BIT(17)

#define REG_EXTKEYINPUT (*(vuint16*)0x04000136)

extern u32 valueBits;
extern s32 mainScreen;
extern u8 consoleModel;

extern vu32* volatile sharedAddr;
// extern bool returnToMenu;
extern int afterSwapTimer;

extern struct IgmText *igmText;

extern void reset(const bool downloadedSrl);
extern void dumpRam(void);
extern void returnToLoader(bool reboot);
extern void prepareScreenshot(void);
extern void saveScreenshot(void);
extern void prepareManual(void);
extern void readManual(int line);
extern void restorePreManual(void);
extern void saveMainScreenSetting(void);
extern void loadInGameMenu(void);
extern void unloadInGameMenu(void);

extern u16 biosRead16(u32 addr);

void biosRead(void* dst, const void* src, u32 len)
{
	u16* _dst = (u16*)dst;
	
	for (u32 i = 0; i < len; i+=2)
	{
		_dst[i>>1] = biosRead16(((u32)src) + i);
	}
}

volatile int timeTillStatusRefresh = 7;

#ifndef TWLSDK
// RetroAchievements unlock chime while the popup is up (the game is paused).
// The game's channels are silenced one by one instead of through the master
// volume, and a free tone channel (8-13) plays a short rising arpeggio that
// fades out.  All is put back afterwards.
#define RA_CHIME_FRAMES 40
#define RA_CHIME_VOLUME 90
// Tone channel timer for a frequency: 33.513982 MHz / 2, 8 steps a period
#define PSG_TIMER(hz) ((u16)(0x10000 - 16756991 / 8 / (hz)))
#define SOUND_VOL_BYTE(n) (*(vu8*)(0x04000400 + ((n) << 4)))

static const struct { u8 frame; u16 timer; } raChimeNotes[] = {
	{ 0, PSG_TIMER(784) },	// G5
	{ 4, PSG_TIMER(1175) },	// D6
	{ 8, PSG_TIMER(1568) },	// G6, then fades out
};
static u8 raChimeVolumes[16];
static int raChimeChannel;

static void raChimeStart(void) {
	raChimeChannel = -1;
	for (int i = 0; i < 16; i++) {
		raChimeVolumes[i] = SOUND_VOL_BYTE(i);
		SOUND_VOL_BYTE(i) = 0;
		if (raChimeChannel < 0 && i >= 8 && i <= 13 && !(SCHANNEL_CR(i) & SCHANNEL_ENABLE)) {
			raChimeChannel = i;
		}
	}
	REG_MASTER_VOLUME = 127;
}

static void raChimeTick(int frame) {
	const int ch = raChimeChannel;
	if (ch < 0 || frame > RA_CHIME_FRAMES) {
		return;
	}
	if (frame == RA_CHIME_FRAMES) {
		SCHANNEL_CR(ch) = 0;
		return;
	}
	for (unsigned n = 0; n < sizeof(raChimeNotes) / sizeof(raChimeNotes[0]); n++) {
		if (raChimeNotes[n].frame == frame) {
			SCHANNEL_TIMER(ch) = raChimeNotes[n].timer;
			SCHANNEL_CR(ch) = SCHANNEL_ENABLE | SOUND_FORMAT_PSG | (2 << 24) | SOUND_PAN(64) | RA_CHIME_VOLUME; // 37.5% duty
		}
	}
	const int last = raChimeNotes[sizeof(raChimeNotes) / sizeof(raChimeNotes[0]) - 1].frame;
	if (frame > last) {
		SOUND_VOL_BYTE(ch) = RA_CHIME_VOLUME * (RA_CHIME_FRAMES - frame) / (RA_CHIME_FRAMES - last);
	}
}

static void raChimeStop(void) {
	if (raChimeChannel >= 0) {
		SCHANNEL_CR(raChimeChannel) = 0;
	}
	for (int i = 0; i < 16; i++) {
		if (i != raChimeChannel) {
			SOUND_VOL_BYTE(i) = raChimeVolumes[i];
		}
	}
	REG_MASTER_VOLUME = 0; // inGameMenu() turns it back up on leaving
}
#endif

// mode: 'MENU', or RA_POPUP_MAGIC / RA_MENU_MAGIC for a RetroAchievements
// unlock popup or achievements list (see ra_popup.h)
void inGameMenu(u32 mode) {
	// returnToMenu = false;
	sharedAddr[4] = mode;
	const u32 errorBak = sharedAddr[0];
	IPC_SendSync(0x9);
	REG_MASTER_VOLUME = 0;
	int oldIME = enterCriticalSection();

	loadInGameMenu();

	int timeOut = 0;
	while (sharedAddr[5] != 0x59444552) { // 'REDY'
		while (REG_VCOUNT != 191) swiDelay(100);
		while (REG_VCOUNT == 191) swiDelay(100);

		timeOut++;
		if (timeOut == 60*2) {
			returnToLoader(true);
			timeOut = 0;
		}
	}

	if (sharedAddr[4] == 0x554E454D || sharedAddr[4] == RA_POPUP_MAGIC || sharedAddr[4] == RA_MENU_MAGIC) {
		bool exitMenu = false;
		#ifndef TWLSDK
		int raFrame = 0;
		if (mode == RA_POPUP_MAGIC) {
			raChimeStart();
		}
		#endif
		while (!exitMenu) {
			#ifndef TWLSDK
			if (mode == RA_POPUP_MAGIC) {
				raChimeTick(raFrame++);
			}
			#endif
			sharedAddr[5] = ~REG_KEYINPUT & 0x3FF;
			sharedAddr[5] |= ((~REG_EXTKEYINPUT & 0x3) << 10) | ((~REG_EXTKEYINPUT & 0xC0) << 6);
			if ((REG_EXTKEYINPUT & BIT(7)) && (valueBits & sleepMode) && (consoleModel < 2)) {
				// Save current power state.
				int power = readPowerManagement(PM_CONTROL_REG);
				// Set sleep LED. (Does not work)
				writePowerManagement(PM_CONTROL_REG, PM_LED_CONTROL(1));

				// Power down till we get our interrupt.
				swiSleep();

				while (REG_EXTKEYINPUT & BIT(7)) {
					//100ms
					swiDelay(838000);
				}

				// Restore power state.
				writePowerManagement(PM_CONTROL_REG, power);
			}
			timeTillStatusRefresh++;
			if (timeTillStatusRefresh >= 8) {
				timeTillStatusRefresh = 0;

				u8 brightness = i2cReadRegister(I2C_PM, 0x41);
				u32 pmControl = readPowerManagement(PM_CONTROL_REG);
				if(brightness && !(pmControl & 0xC)) { // Turn on backlights if SELECT+volume used
					pmControl |= 0xC;
					writePowerManagement(PM_CONTROL_REG, pmControl);
				}

				sharedAddr[6] = i2cReadRegister(I2C_PM, I2CREGPM_BATTERY); // Battery
				sharedAddr[6] |= (brightness + ((pmControl & 0xC) != 0)) << 8; // Brightness
				sharedAddr[6] |= i2cReadRegister(I2C_PM, I2CREGPM_VOL) << 16; // Volume

				RTCtime dstime;
				rtcGetTimeAndDate((uint8 *)&dstime);
				sharedAddr[7] = dstime.hours;
				sharedAddr[8] = dstime.minutes;
				sharedAddr[7] += 0x10000000; // Set time receive flag
			}

			while (REG_VCOUNT != 191) swiDelay(100);
			while (REG_VCOUNT == 191) swiDelay(100);

			#ifndef TWLSDK
			// RetroAchievements hardcore: no refresh-rate change, RAM dump,
			// RAM viewer or editor (the menu hides them too)
			extern bool raHardcoreActive(void);
			if (raHardcoreActive()) {
				const u32 cmd = sharedAddr[4];
				if (cmd == 0x41535046 || cmd == 0x444D4152 || cmd == 0x524D4152 || cmd == 0x574D4152) {
					sharedAddr[0] = 0xFFFFFFFF; // refused
					sharedAddr[4] = 0x554E454D; // MENU
				}
			}
			#endif

			switch (sharedAddr[4]) {
				/* case 0x54495845: // EXIT
					exitMenu = true;
					break; */
				case 0x54455352: // RSET
					exitMenu = true;
					timeTillStatusRefresh = 7;
					unloadInGameMenu();
					#ifdef TWLSDK
					i2cWriteRegister(0x4A, 0x12, 0x01);
					#endif
					reset(false);
					break;
				case 0x54495551: // QUIT
					unloadInGameMenu();
					returnToLoader(false);
					exitMenu = true;
					break;
				case 0x53435049: // IPCS
					mainScreen = sharedAddr[0];
					saveMainScreenSetting();
					afterSwapTimer = 0;
					break;
				case 0x41535046: // FPSA
					#ifndef TWLSDK
					extern fpsa_t sActiveFpsa;

					const u32 num = sharedAddr[0];
					if (num == 60000) {
						fpsa_stop(&sActiveFpsa);
					} else {
						fpsa_init(&sActiveFpsa);
						fpsa_setTargetFpsFraction(&sActiveFpsa, num, num >= 1000 ? 1001 : 1);
						fpsa_start(&sActiveFpsa);
					}
					#else
					sharedAddr[0] = 0xFFFFFFFF;
					#endif
					break;
				#ifndef TWLSDK
				case 0x43484152: // RAHC: RetroAchievements softcore (0) / hardcore (1)
				{
					extern void raSetHardcore(bool on);
					raSetHardcore(sharedAddr[0] != 0);
					break;
				}
				#endif
				case 0x444D4152: // RAMD
					dumpRam();
					exitMenu = true;
					break;
				/* case 0x50455453: // STEP
					returnToMenu = true;
					exitMenu = true;
					break; */
				case 0x50505353: // SSPP
					prepareScreenshot();
					break;
				case 0x544F4853: // SHOT
					saveScreenshot();
					break;
				case 0x4E414D50: // PMAN
					prepareManual();
					break;
				case 0x554E414D: // MANU
					readManual(sharedAddr[0]);
					break;
				case 0x4E414D52: // RMAN
					restorePreManual();
					break;
				case 0x524D4152: // RAMR
					u32* dst = (u32*)((u32)sharedAddr[0]);
					u32* src = (u32*)((u32)sharedAddr[1]);
					for (int i = 0; i < 0xC0/8; i++) {
						if ((u32)src >= 0x8000) {
							tonccpy(dst, src, 8);
						} else {
							biosRead(dst, src, 8);
						}
						dst++;
						dst++;
						src++;
						src++;
					}
					break;
				case 0x574D4152: // RAMW
					if (sharedAddr[1]+sharedAddr[2] >= 0x8000) {
						tonccpy((u8*)((u32)sharedAddr[1])+sharedAddr[2], (u8*)((u32)sharedAddr[0])+sharedAddr[2], 1);
					}
					break;
				case 0x4554494C: // LITE
					if(sharedAddr[0] == 0) {
						writePowerManagement(PM_CONTROL_REG, readPowerManagement(PM_CONTROL_REG) & ~0xC);
					} else {
						i2cWriteRegister(I2C_PM, 0x41, sharedAddr[0] - 1);
						writePowerManagement(PM_CONTROL_REG, readPowerManagement(PM_CONTROL_REG) | 0xC);
					}
					timeTillStatusRefresh = 7;
					break;
				case 0x554C4F56: // VOLU
					i2cWriteRegister(I2C_PM, I2CREGPM_VOL, sharedAddr[0]);
					timeTillStatusRefresh = 7;
					break;
				default:
					break;
			}

			if (sharedAddr[4] == 0x54495845) { // EXIT
				// returnToMenu = (sharedAddr[1]);
				exitMenu = true;
			} else if (!exitMenu) {
				sharedAddr[4] = 0x554E454D; // MENU
			}
		}
		#ifndef TWLSDK
		if (mode == RA_POPUP_MAGIC) {
			raChimeStop();
		}
		#endif
	}

	sharedAddr[0] = errorBak;
	sharedAddr[4] = 0;
	sharedAddr[7] -= 0x10000000; // Clear time receive flag
	timeTillStatusRefresh = 7;

	unloadInGameMenu();

	leaveCriticalSection(oldIME);
	REG_MASTER_VOLUME = 127;
}

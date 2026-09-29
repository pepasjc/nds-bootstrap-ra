/*
	NitroHax -- Cheat tool for the Nintendo DS
	Copyright (C) 2008  Michael "Chishm" Chisholm

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful, 
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <nds/ndstypes.h>
#include <nds/fifomessages.h>
#include <nds/dma.h>
#include <nds/ipc.h>
#include <nds/system.h>
#include <nds/interrupts.h>
#include <nds/input.h>
#include <nds/timers.h>
#include <nds/arm7/audio.h>
#include <nds/arm7/i2c.h>
#include <nds/arm7/serial.h> // in-game sending: the WiFi settings in flash
#include <nds/memory.h> // tNDSHeader
#include <nds/debug.h>

#include "ndma.h"
#include "dmaTwl.h"
#include "tonccpy.h"
#include "my_sdmmc.h"
#include "my_fat.h"
#include "locations.h"
#include "module_params.h"
#include "unpatched_funcs.h"
#include "debug_file.h"
#include "cardengine.h"
#include "fpsAdjust.h"
#include "nds_header.h"
#include "igm_text.h"
#include "ra_popup.h"
#include "ra_engine.h"
#include <nds/arm7/clock.h>

#ifndef TWLSDK
// Patcher
#include "common.h"
#include "decompress.h"
#include "patch.h"
#include "find.h"
#include "hook.h"
#endif

// TWL soft-reset
#include "sr_data_error.h"      // For showing an error screen

#define gameOnFlashcard BIT(0)
#define saveOnFlashcard BIT(1)
#define eSdk2 BIT(1)
#define ROMinRAM BIT(3)
#define dsiMode BIT(4)
#define b_dsiSD BIT(5)
#define preciseVolumeControl BIT(6)
#define powerCodeOnVBlank BIT(7)
#define delayWrites BIT(8)
#define igmAccessible BIT(9)
#define quitOnFlashcard BIT(10)
#define slowSoftReset BIT(11)
#define wideCheatUsed BIT(12)
#define isSdk5 BIT(13)
#define hasVramWifiBinary BIT(14)
#define twlTouch BIT(15)
#define cloneboot BIT(16)
#define sleepMode BIT(17)
#define b_dsiBios BIT(18)
#define bootstrapOnFlashcard BIT(19)
#define ndmaDisabled BIT(20)
#define isDlp BIT(21)
#define useColorLut BIT(22)
#define clearRamOnReset BIT(23)
#define i2cBricked BIT(30)
#define scfgLocked BIT(31)

#define	REG_EXTKEYINPUT	(*(vuint16*)0x04000136)
#define	REG_WIFIIRQ	(*(vuint16*)0x04808012)

extern u32 ce7;

static const char *unlaunchAutoLoadID = "AutoLoadInfo";

extern u32 srBackendId[2];
extern u32 srFrontendId[2];

extern void ndsCodeStart(u32* addr);
extern int tryLockMutex(int* addr);
extern int lockMutex(int* addr);
extern int unlockMutex(int* addr);

extern vu32* volatile cardStruct;
extern u32 cheatEngineAddr;
extern u32 quitFileCluster;
extern u32 fileCluster;
extern u32 saveCluster;
extern u32 saveSize;
extern u32 patchOffsetCacheFileCluster;
extern u32 srParamsCluster;
extern u32 ramDumpCluster;
extern u32 screenshotCluster;
extern u32 pageFileCluster;
extern u32 manualCluster;
extern module_params_t* moduleParams;
extern u32 valueBits;
extern s32 mainScreen;
extern u32* languageAddr;
extern u8 language;
extern u8 consoleModel;
extern u8 romRead_LED;
extern u8 dmaRomRead_LED;
extern u8 remappedKeys[12];
extern u16 igmHotkey;
extern u16 screenSwapHotkey;

#ifdef TWLSDK
vu32* volatile sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK5;
#else
vu32* volatile sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK1;
#endif

struct IgmText *igmText = (struct IgmText *)INGAME_MENU_LOCATION;

static bool initialized = false;
static bool driveInited = false;
#ifdef TWLSDK
static bool sixInHeader = false;
#endif
static bool bootloaderCleared = false;
static bool funcsUnpatched = false;
//static bool initializedIRQ = false;
//static bool calledViaIPC = false;
//static bool ipcSyncHooked = false;
//static bool dmaLed = false;
static bool powerLedChecked = false;
static bool powerLedIsPurple = false;
static bool wifiIrq = false;
static int wifiIrqTimer = 0;
//static bool saveInRam = false;

#ifdef TWLSDK
static aFile* romFile = (aFile*)ROM_FILE_LOCATION_TWLSDK;
static aFile* savFile = (aFile*)SAV_FILE_LOCATION_TWLSDK;
//static aFile* gbaFile = (aFile*)GBA_FILE_LOCATION_TWLSDK;
static aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION_TWLSDK;
static aFile* sharedFontFile = (aFile*)FONT_FILE_LOCATION_TWLSDK;
#else
#ifdef ALTERNATIVE
static aFile* romFile = (aFile*)ROM_FILE_LOCATION_ALT;
static aFile* savFile = (aFile*)SAV_FILE_LOCATION_ALT;
//static aFile* gbaFile = (aFile*)GBA_FILE_LOCATION_ALT;
static aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION_ALT;
#else
static aFile* romFile = (aFile*)ROM_FILE_LOCATION;
static aFile* savFile = (aFile*)SAV_FILE_LOCATION;
//static aFile* gbaFile = (aFile*)GBA_FILE_LOCATION;
static aFile* apFixOverlaysFile = (aFile*)OVL_FILE_LOCATION;
#endif
#endif
static aFile patchOffsetCacheFile;
static aFile ramDumpFile;
static aFile srParamsFile;
static aFile screenshotFile;
static aFile pageFile;
static aFile manualFile;

static int saveTimer = 0;

static int languageTimer = 0;
static int swapTimer = 0;
int afterSwapTimer = 0;
static int returnTimer = 0;
static int softResetTimer = 0;
// static int ramDumpTimer = 0;
static int noI2CVolLevel = 127; // Volume workaround for bricked I2C chips
static int volumeAdjustDelay = 0;
static bool volumeAdjustActivated = false;

#ifndef TWLSDK
fpsa_t sActiveFpsa;
#endif

//static bool ndmaUsed = false;

// static int cardEgnineCommandMutex = 0;
static int saveMutex = 0;

bool returnToMenu = false;

#ifdef TWLSDK
static tNDSHeader* ndsHeader = (tNDSHeader*)NDS_HEADER_SDK5;
static PERSONAL_DATA* personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER_SDK5-0x180);
#else
static tNDSHeader* ndsHeader = (tNDSHeader*)NDS_HEADER;
static PERSONAL_DATA* personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER-0x180);
#endif

extern u32 romLocation;

extern u32 romMapLines;
// 0: ROM part start, 1: ROM part start in RAM, 2: ROM part end in RAM
extern u32 romMap[][3];

u32 currentSrlAddr = 0;

void i2cIRQHandler(void);

static void unlaunchSetFilename(void) {
	const u8* filename = 
	#ifdef TWLSDK
	(u8*)(ce7+0x8400);
	#else
	(u8*)(ce7+0xF400);
	#endif

	if (filename[0] == 0) return;

	tonccpy((u8*)0x02000800, unlaunchAutoLoadID, 12);
	*(u16*)(0x0200080C) = 0x3F0;		// Unlaunch Length for CRC16 (fixed, must be 3F0h)
	*(u16*)(0x0200080E) = 0;			// Unlaunch CRC16 (empty)
	*(u32*)(0x02000810) = (BIT(0) | BIT(1));		// Load the title at 2000838h
													// Use colors 2000814h
	*(u16*)(0x02000814) = 0x7FFF;		// Unlaunch Upper screen BG color (0..7FFFh)
	*(u16*)(0x02000816) = 0x7FFF;		// Unlaunch Lower screen BG color (0..7FFFh)
	toncset((u8*)0x02000818, 0, 0x20+0x208+0x1C0);		// Unlaunch Reserved (zero)
	int i2 = 0;
	for (int i = 0; i < 256; i++) {
		*(u8*)(0x02000838+i2) = filename[i];		// Unlaunch Device:/Path/Filename.ext (16bit Unicode,end by 0000h)
		i2 += 2;
	}
	*(u16*)(0x0200080E) = swiCRC16(0xFFFF, (void*)0x02000810, 0x3F0);		// Unlaunch CRC16
}

static void readSoftResetId(const bool front) {
	if (front) {
		if (srFrontendId[0] == 0 && srFrontendId[1] == 0) return;
	} else {
		if (srBackendId[0] == 0 && srBackendId[1] == 0) return;
	}

	// Use soft-reset ID
	*(u32*)(0x02000300) = 0x434E4C54;	// 'CNLT'
	*(u16*)(0x02000304) = 0x1801;
	*(u32*)(0x02000308) = 0;
	*(u32*)(0x0200030C) = 0;
	*(u32*)(0x02000310) = front ? srFrontendId[0] : srBackendId[0];
	*(u32*)(0x02000314) = front ? srFrontendId[1] : srBackendId[1];
	*(u32*)(0x02000318) = *(u32*)(0x02000314) == 0x00030000 ? 0x13 : 0x17;
	*(u32*)(0x0200031C) = 0;
	*(u16*)(0x02000306) = swiCRC16(0xFFFF, (void*)0x02000308, 0x18);
}

// Alternative to swiWaitForVBlank()
static inline void waitFrames(int count) {
	for (int i = 0; i < count; i++) {
		while (REG_VCOUNT != 191);
		while (REG_VCOUNT == 191);
	}
}

static inline bool isSdEjected(void) {
	return (*(vu32*)(0x400481C) & BIT(3));
}

static void driveInitialize(void) {
	if (driveInited) {
		return;
	}

	if (valueBits & b_dsiSD) {
		if (sdmmc_read16(REG_SDSTATUS0) != 0) {
			sdmmc_init();
			SD_Init();
		}
		FAT_InitFiles(false, false);
	}
	if (((valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM)) || (valueBits & saveOnFlashcard)) {
		FAT_InitFiles(false, true);
	}

	getFileFromCluster(&patchOffsetCacheFile, patchOffsetCacheFileCluster, (valueBits & gameOnFlashcard));
	getFileFromCluster(&ramDumpFile, ramDumpCluster, (valueBits & bootstrapOnFlashcard));
	getFileFromCluster(&srParamsFile, srParamsCluster, (valueBits & gameOnFlashcard));
	getFileFromCluster(&screenshotFile, screenshotCluster, (valueBits & bootstrapOnFlashcard));
	getFileFromCluster(&pageFile, pageFileCluster, (valueBits & bootstrapOnFlashcard));
	getFileFromCluster(&manualFile, manualCluster, (valueBits & bootstrapOnFlashcard));

	//romFile = getFileFromCluster(fileCluster);
	//buildFatTableCache(&romFile, 0);
	#ifdef DEBUG	
	if (romFile->fatTableCached) {
		nocashMessage("fat table cached");
	} else {
		nocashMessage("fat table not cached"); 
	}
	#endif
	
	/*if (saveCluster > 0) {
		savFile = getFileFromCluster(saveCluster);
	} else {
		savFile.firstCluster = CLUSTER_FREE;
	}*/

	#ifdef DEBUG		
	aFile myDebugFile;
	getBootFileCluster(&myDebugFile, "NDSBTSRP.LOG", 0, !(valueBits & b_dsiSD));
	enableDebug(&myDebugFile);
	dbg_printf("logging initialized\n");		
	dbg_printf("sdk version :");
	dbg_hexa(moduleParams->sdk_version);		
	dbg_printf("\n");	
	dbg_printf("rom file :");
	dbg_hexa(fileCluster);	
	dbg_printf("\n");	
	dbg_printf("save file :");
	dbg_hexa(saveCluster);	
	dbg_printf("\n");
	#endif

	if (valueBits & ndmaDisabled) {
		sdmmc_lock_ndma_slot();
	}

	sdmmc_set_ndma_slot(0);
	driveInited = true;
}

static void initialize(void) {
	if (initialized) {
		return;
	}

	#ifdef TWLSDK
	if (*(u8*)(DSI_HEADER_SDK5+0x234) == 6) {
		*(u8*)(DSI_HEADER_SDK5+0x234) = 0;
		sixInHeader = true;
	}
	#else
	if (valueBits & isSdk5) {
		sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK5;
		ndsHeader = (tNDSHeader*)NDS_HEADER_SDK5;
		personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER_SDK5-0x180);
	} else {
		sharedAddr = (vu32*)CARDENGINE_SHARED_ADDRESS_SDK1;
		ndsHeader = (tNDSHeader*)NDS_HEADER;
		personalData = (PERSONAL_DATA*)((u8*)NDS_HEADER-0x180);
	}
	#endif

	if (language >= 0 && language <= 7) {
		// Change language
		personalData->language = language;
	}

	if (!bootloaderCleared) {
		toncset((u8*)0x06000000, 0, 0x40000);	// Clear bootloader
		bootloaderCleared = true;
	}

	initialized = true;
}

#ifdef TWLSDK
/*u32 auxIeBak = 0;
u32 sdStatBak = 0;
u32 sdMaskBak = 0;

void bakSdData(void) {
	auxIeBak = REG_AUXIE;
	sdStatBak = *(vu32*)0x400481C;
	sdMaskBak = *(vu32*)0x4004820;

	REG_AUXIE &= ~(1UL << 8);
	*(vu32*)0x400481C = 0;
	*(vu32*)0x4004820 = 0;
}

void restoreSdBakData(void) {
	REG_AUXIE = auxIeBak;
	*(vu32*)0x400481C = sdStatBak;
	*(vu32*)0x4004820 = sdMaskBak;
}*/
#else
bool buttonsRemapped(void) {
	for (int i = 0; i < 12; i++) {
		if (remappedKeys[i] != i) {
			return true;
		}
	}
	return false;
}

static module_params_t* getModuleParams(const tNDSHeader* ndsHeader) {
	//nocashMessage("Looking for moduleparams...\n");

	u32* moduleParamsOffset = findModuleParamsOffset(ndsHeader);

	//module_params_t* moduleParams = (module_params_t*)((u32)moduleParamsOffset - 0x1C);
	return moduleParamsOffset ? (module_params_t*)(moduleParamsOffset - 7) : NULL;
}
#endif

static bool cardReadRAM(u8* dst, u32 src, u32 len/*, int romPartNo*/) {
	if (!(valueBits & ROMinRAM)) {
		return false;
	}

	// Copy directly
	#ifdef TWLSDK
	u32 newSrc = romLocation/*[romPartNo]*/+src;
	if (src > *(u32*)0x02FFE1C0) {
		newSrc -= *(u32*)0x02FFE1CC;
	}
	tonccpy(dst, (u8*)newSrc, len);
	#else
	// tonccpy(dst, (u8*)romLocation/*[romPartNo]*/+src, len);
	u32 newSrc = 0;
	u32 newLen = 0;
	bool srcFound = false;
	int i = 0;
	for (i = 0; i < romMapLines; i++) {
		if (src >= romMap[i][0] && (i == romMapLines-1 || src < romMap[i+1][0])) {
			srcFound = true;
			break;
		}
	}
	if (!srcFound) {
		return false;
	}
	while (len > 0) {
		newSrc = (romMap[i][1]-romMap[i][0])+src;
		if (newSrc >= 0x03000000) {
			return false; // Unable to read from ARM9-exclusive areas
		}
		newLen = len;
		while (newSrc+newLen > romMap[i][2]) {
			newLen--;
		}
		tonccpy(dst, (u8*)newSrc, newLen);
		src += newLen;
		dst += newLen;
		len -= newLen;
		i++;
	}
	#endif
	return true;
}

void reset(const bool downloadedSrl) {
	register int i, reg;

#ifndef TWLSDK
	u32 resetParam = ((valueBits & isSdk5) ? RESET_PARAM_SDK5 : RESET_PARAM);
	if (((valueBits & isDlp) && *(u32*)(NDS_HEADER_SDK5+0xC) == 0) || (valueBits & slowSoftReset) || (*(u32*)(resetParam+0xC) > 0 && (valueBits & isSdk5))) {
		REG_MASTER_VOLUME = 0;
		int oldIME = enterCriticalSection();
		//driveInitialize();
		if (downloadedSrl) {
			*(u32*)resetParam = 0;
			*(u32*)(resetParam+8) = 0x44414F4C; // 'LOAD'
			fileWrite((char*)ndsHeader, &pageFile, 0x2BFE00, 0x160);
			fileWrite((char*)ndsHeader->arm9destination, &pageFile, 0x14000, ndsHeader->arm9binarySize);
			fileWrite((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, ndsHeader->arm7binarySize);
		}
		fileWrite((char*)resetParam, &srParamsFile, 0, 0x10);
		fileWrite((char*)resetParam+0x20, &srParamsFile, 0x10, 0x40);
		toncset((u32*)0x02000000, 0, 0x400);
		*(u32*)0x02000000 = BIT(3);
		*(u32*)0x02000004 = 0x54455352; // 'RSET'
		unlaunchSetFilename();
		readSoftResetId(false);
		i2cWriteRegister(0x4A, 0x70, 0x01);
		i2cWriteRegister(0x4A, 0x11, 0x01);			// Reboot game
		leaveCriticalSection(oldIME);
		while (1);
	}

	if (!(valueBits & isDlp)) {
#endif

	REG_IME = 0;

	for (i = 0; i < 16; i++) {
		SCHANNEL_CR(i) = 0;
		SCHANNEL_TIMER(i) = 0;
		SCHANNEL_SOURCE(i) = 0;
		SCHANNEL_LENGTH(i) = 0;
	}

	REG_SOUNDCNT = 0;
	REG_SNDCAP0CNT = 0;
	REG_SNDCAP1CNT = 0;

	REG_SNDCAP0DAD = 0;
	REG_SNDCAP0LEN = 0;
	REG_SNDCAP1DAD = 0;
	REG_SNDCAP1LEN = 0;

	// Clear out ARM7 DMA channels and timers
	for (i = 0; i < 4; i++) {
		DMA_CR(i) = 0;
		DMA_SRC(i) = 0;
		DMA_DEST(i) = 0;
		TIMER_CR(i) = 0;
		TIMER_DATA(i) = 0;
	}

	// Clear out FIFO
	REG_IPC_SYNC = 0;
	REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_SEND_CLEAR;
	REG_IPC_FIFO_CR = 0;

	REG_IE = 0;
	REG_IF = ~0;
	*(vu32*)0x0380FFFC = 0;  // IRQ_HANDLER ARM7 version
	*(vu32*)0x0380FFF8 = 0; // VBLANK_INTR_WAIT_FLAGS, ARM7 version
	REG_POWERCNT = 1;  // Turn off power to stuff

	funcsUnpatched = false;

#ifndef TWLSDK
	}
#endif

	initialized = false;
	//ipcSyncHooked = false;
	languageTimer = 0;
	unlockMutex(&saveMutex);

	#ifndef TWLSDK
	if ((valueBits & isDlp) || currentSrlAddr != *(u32*)(resetParam+0xC) || downloadedSrl) {
		currentSrlAddr = *(u32*)(resetParam+0xC);
		if ((valueBits & isDlp) || downloadedSrl) {
			// ndmaCopyWordsAsynch(1, (u32*)0x022C0000, ndsHeader->arm7destination, ndsHeader->arm7binarySize);
		} else {
			if (!cardReadRAM((u8*)ndsHeader, currentSrlAddr, 0x160)) {
				fileRead((char*)ndsHeader, romFile, currentSrlAddr, 0x160);
			}
			if (!cardReadRAM((u8*)ndsHeader->arm9destination, currentSrlAddr+ndsHeader->arm9romOffset, ndsHeader->arm9binarySize)) {
				fileRead((char*)ndsHeader->arm9destination, romFile, currentSrlAddr+ndsHeader->arm9romOffset, ndsHeader->arm9binarySize);
			}
			if (!cardReadRAM((u8*)ndsHeader->arm7destination, currentSrlAddr+ndsHeader->arm7romOffset, ndsHeader->arm7binarySize)) {
				fileRead((char*)ndsHeader->arm7destination, romFile, currentSrlAddr+ndsHeader->arm7romOffset, ndsHeader->arm7binarySize);
			}
		}

		moduleParams = getModuleParams(ndsHeader);
		/*dbg_printf("sdk_version: ");
		dbg_hexa(moduleParams->sdk_version);
		dbg_printf("\n");*/ 
		if (moduleParams->sdk_version > 0x5000000) {
			valueBits |= isSdk5;
		} else {
			valueBits &= ~isSdk5;
		}
		/* if ((moduleParams->sdk_version >= 0x2008000 && moduleParams->sdk_version != 0x2012774) || moduleParams->sdk_version == 0x20029A8) {
			valueBits &= ~eSdk2;
		} else {
			valueBits |= eSdk2;
		} */

		ensureBinaryDecompressed(ndsHeader, moduleParams);

		const bool buttonsRemappedBool = buttonsRemapped();

		patchCardNdsArm9(
			(cardengineArm9*)CARDENGINEI_ARM9_LOCATION,
			ndsHeader,
			moduleParams,
			1,
			buttonsRemappedBool
		);
		patchCardNdsArm7(
			(cardengineArm7*)ce7,
			ndsHeader,
			moduleParams,
			buttonsRemappedBool
		);

		hookNdsRetailArm7(
			(cardengineArm7*)ce7,
			ndsHeader
		);
		hookNdsRetailArm9(
			(cardengineArm9*)CARDENGINEI_ARM9_LOCATION,
			ndsHeader
		);

		extern u32 iUncompressedSize;

		fileWrite((char*)ndsHeader->arm9destination, &pageFile, 0x14000, iUncompressedSize);
		fileWrite((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, ndsHeader->arm7binarySize);
		fileWrite((char*)&iUncompressedSize, &pageFile, 0x5FFFF0, sizeof(u32));
		fileWrite((char*)&ndsHeader->arm7binarySize, &pageFile, 0x5FFFF4, sizeof(u32));
		/* } else {
			*(u32*)ARM9_DEC_SIZE_LOCATION = iUncompressedSize;
			ndmaCopyWordsAsynch(0, ndsHeader->arm9destination, (char*)ndsHeader->arm9destination+0x400000, *(u32*)ARM9_DEC_SIZE_LOCATION);
			ndmaCopyWordsAsynch(1, ndsHeader->arm7destination, (char*)DONOR_ROM_ARM7_LOCATION, ndsHeader->arm7binarySize);
			while (ndmaBusy(0) || ndmaBusy(1));
		} */
		if ((valueBits & isDlp) && !(valueBits & isSdk5)) {
			tonccpy((u8*)0x027FF000, (u8*)0x02FFF000, 0x1000);
		}
		valueBits &= ~isDlp;
	} else {
		//driveInitialize();

		u32 iUncompressedSize = 0;
		u32 newArm7binarySize = 0;
		fileRead((char*)&iUncompressedSize, &pageFile, 0x5FFFF0, sizeof(u32));
		fileRead((char*)&newArm7binarySize, &pageFile, 0x5FFFF4, sizeof(u32));
		if (valueBits & clearRamOnReset) {
			dma_twlFill32Async(1, 0, (void*)0x02000000+iUncompressedSize, 0x3E0000-iUncompressedSize);
		}
		fileRead((char*)ndsHeader->arm9destination, &pageFile, 0x14000, iUncompressedSize);
		if (valueBits & clearRamOnReset) {
			while (ndmaBusy(1));
		}
		fileRead((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, newArm7binarySize);
	} /* else {
		ndmaCopyWordsAsynch(0, (char*)ndsHeader->arm9destination+0x400000, ndsHeader->arm9destination, *(u32*)ARM9_DEC_SIZE_LOCATION);
		ndmaCopyWordsAsynch(1, (char*)DONOR_ROM_ARM7_LOCATION, ndsHeader->arm7destination, ndsHeader->arm7binarySize);
		while (ndmaBusy(0) || ndmaBusy(1));
	} */
	#else
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();

	//driveInitialize();

	u32 iUncompressedSize = 0;
	u32 iUncompressedSizei = 0;
	u32 newArm7binarySize = 0;
	u32 newArm7ibinarySize = 0;

	fileRead((char*)&iUncompressedSize, &pageFile, 0x5FFFF0, sizeof(u32));
	fileRead((char*)&newArm7binarySize, &pageFile, 0x5FFFF4, sizeof(u32));
	fileRead((char*)&iUncompressedSizei, &pageFile, 0x5FFFF8, sizeof(u32));
	fileRead((char*)&newArm7ibinarySize, &pageFile, 0x5FFFFC, sizeof(u32));
	fileRead((char*)ndsHeader->arm9destination, &pageFile, 0x14000, iUncompressedSize);
	fileRead((char*)ndsHeader->arm7destination, &pageFile, 0x2C0000, newArm7binarySize);
	fileRead((char*)(*(u32*)0x02FFE1C8), &pageFile, 0x300000, iUncompressedSizei);
	fileRead((char*)(*(u32*)0x02FFE1D8), &pageFile, 0x580000, newArm7ibinarySize);

	if (sixInHeader) {
		*(u8*)(DSI_HEADER_SDK5+0x234) = 6;
	}
	//if (doBak) restoreSdBakData();
	#endif
	toncset((char*)((valueBits & isSdk5) ? 0x02FFFD80 : 0x027FFD80), 0, 0x80);
	toncset((char*)((valueBits & isSdk5) ? 0x02FFFF80 : 0x027FFF80), 0, 0x80);

	sharedAddr[0] = 0x44414F4C; // 'LOAD'

	for (i = 0; i < 4; i++) {
		for(reg=0; reg<0x1c; reg+=4)*((vu32*)(0x04004104 + ((i*0x1c)+reg))) = 0;//Reset NDMA.
	}

	while (sharedAddr[0] != 0x544F4F42) { // 'BOOT'
		while (REG_VCOUNT != 191) swiDelay(100);
		while (REG_VCOUNT == 191) swiDelay(100);
	}

	// Start ARM7
	ndsCodeStart(ndsHeader->arm7executeAddress);
}

static void cardReadLED(const bool on, const bool dmaLed) {
	if (!(valueBits & i2cBricked) && consoleModel < 2) { /* Proceed below */ } else { return; }

	/* static bool ledIsOn = false;
	static bool dmaLedIsOn = false;
	if (dmaLed ? dmaLedIsOn == on : ledIsOn == on) {
		return;
	}
	dmaLed
		? (dmaLedIsOn = on)
		: (ledIsOn = on); */

	if (dmaRomRead_LED == -1) dmaRomRead_LED = romRead_LED;
	if (!powerLedChecked && (romRead_LED || dmaRomRead_LED)) {
		const u8 byte = i2cReadRegister(0x4A, 0x63);
		powerLedIsPurple = (byte == 0xFF);
		powerLedChecked = true;
	}
	if (on) {
		switch(dmaLed ? dmaRomRead_LED : romRead_LED) {
			case 0:
			default:
				break;
			case 1:
				i2cWriteRegister(0x4A, 0x30, 0x13);    // Turn WiFi LED on
				break;
			case 2:
				i2cWriteRegister(0x4A, 0x63, powerLedIsPurple ? 0x00 : 0xFF);    // Turn power LED purple
				break;
			case 3:
				i2cWriteRegister(0x4A, 0x31, 0x01);    // Turn Camera LED on
				break;
		}
	} else {
		switch(dmaLed ? dmaRomRead_LED : romRead_LED) {
			case 0:
			default:
				break;
			case 1:
				i2cWriteRegister(0x4A, 0x30, 0x12);    // Turn WiFi LED off
				break;
			case 2:
				i2cWriteRegister(0x4A, 0x63, powerLedIsPurple ? 0xFF : 0x00);    // Revert power LED to normal
				break;
			case 3:
				i2cWriteRegister(0x4A, 0x31, 0x00);    // Turn Camera LED off
				break;
		}
	}
}

/*static void asyncCardReadLED(bool on) {
	if (consoleModel < 2) {
		if (on) {
			switch(romread_LED) {
				case 0:
				default:
					break;
				case 1:
					i2cWriteRegister(0x4A, 0x63, 0xFF);    // Turn power LED purple
					break;
				case 2:
					i2cWriteRegister(0x4A, 0x30, 0x13);    // Turn WiFi LED on
					break;
			}
		} else {
			switch(romread_LED) {
				case 0:
				default:
					break;
				case 1:
					i2cWriteRegister(0x4A, 0x63, 0x00);    // Revert power LED to normal
					break;
				case 2:
					i2cWriteRegister(0x4A, 0x30, 0x12);    // Turn WiFi LED off
					break;
			}
		}
	}
}*/

extern void inGameMenu(u32 mode);

static inline void rebootConsole(void) {
	if (valueBits & i2cBricked) {
		u8 readCommand = readPowerManagement(0x10);
		readCommand |= BIT(0);
		writePowerManagement(0x10, readCommand); // Reboot console
		return;
	}
	i2cWriteRegister(0x4A, 0x70, 0x01);
	i2cWriteRegister(0x4A, 0x11, 0x01);
}

void forceGameReboot(void) {
	toncset((u32*)0x02000000, 0, 0x400);
	*(u32*)0x02000000 = BIT(3);
	*(u32*)0x02000004 = 0x54455352; // 'RSET'
	sharedAddr[4] = 0x57534352;
	IPC_SendSync(0x8);
	u32 clearBuffer = 0;
	#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
	#endif
	//driveInitialize();
	fileWrite((char*)&clearBuffer, &srParamsFile, 0, 0x4);
  	#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
	#endif
	if (consoleModel < 2) {
		unlaunchSetFilename();
		readSoftResetId(false);
		waitFrames(5);							// Wait for DSi screens to stabilize
	} else {
		readSoftResetId(false);
		waitFrames(1);
	}
	rebootConsole();		// Force-reboot game
}

#ifdef TWLSDK
/* static void initMBK_dsiMode(void) {
	// This function has no effect with ARM7 SCFG locked
	*(vu32*)REG_MBK1 = *(u32*)0x02FFE180;
	*(vu32*)REG_MBK2 = *(u32*)0x02FFE184;
	*(vu32*)REG_MBK3 = *(u32*)0x02FFE188;
	*(vu32*)REG_MBK4 = *(u32*)0x02FFE18C;
	*(vu32*)REG_MBK5 = *(u32*)0x02FFE190;
	REG_MBK6 = *(u32*)0x02FFE1A0;
	REG_MBK7 = *(u32*)0x02FFE1A4;
	REG_MBK8 = *(u32*)0x02FFE1A8;
	REG_MBK9 = *(u32*)0x02FFE1AC;
} */

extern bool dldiPatchBinary (unsigned char *binData, u32 binSize);
#endif

#ifndef TWLSDK
static bool raQuitThroughSync(void);
#endif

void returnToLoader(bool reboot) {
	toncset((u32*)0x02000000, 0, 0x400);
	*(u32*)0x02000000 = BIT(0) | BIT(1) | BIT(2);
	*(u32*)0x02000004 = 0x54455352; // 'RSET'
	sharedAddr[4] = 0x57534352;
#ifdef TWLSDK
	u32 twlCfgLoc = *(u32*)0x02FFFDFC;
	if (twlCfgLoc != 0x02000400) {
		tonccpy((u8*)0x02000400, (u8*)twlCfgLoc, 0x128);
	}

	if (reboot || !(valueBits & b_dsiBios) || ((valueBits & twlTouch) && !(*(u8*)0x02FFE1BF & BIT(0))) || ((valueBits & b_dsiSD) && (valueBits & wideCheatUsed))) {
		if (consoleModel < 2) {
			unlaunchSetFilename();
			readSoftResetId(true);
			waitFrames(5);							// Wait for DSi screens to stabilize
		} else {
			readSoftResetId(true);
			waitFrames(1);
		}
		rebootConsole();
	}

	register int i, reg;

	REG_IME = 0;

	for (i = 0; i < 16; i++) {
		SCHANNEL_CR(i) = 0;
		SCHANNEL_TIMER(i) = 0;
		SCHANNEL_SOURCE(i) = 0;
		SCHANNEL_LENGTH(i) = 0;
	}

	REG_SOUNDCNT = 0;
	REG_SNDCAP0CNT = 0;
	REG_SNDCAP1CNT = 0;

	REG_SNDCAP0DAD = 0;
	REG_SNDCAP0LEN = 0;
	REG_SNDCAP1DAD = 0;
	REG_SNDCAP1LEN = 0;

	// Clear out ARM7 DMA channels and timers
	for (i = 0; i < 4; i++) {
		DMA_CR(i) = 0;
		DMA_SRC(i) = 0;
		DMA_DEST(i) = 0;
		TIMER_CR(i) = 0;
		TIMER_DATA(i) = 0;
	}

	// Clear out FIFO
	REG_IPC_SYNC = 0;
	REG_IPC_FIFO_CR = IPC_FIFO_ENABLE | IPC_FIFO_SEND_CLEAR;
	REG_IPC_FIFO_CR = 0;

	REG_IE = 0;
	REG_IF = ~0;
	REG_AUXIE = 0;
	REG_AUXIF = ~0;
	*(vu32*)0x0380FFFC = 0;  // IRQ_HANDLER ARM7 version
	*(vu32*)0x0380FFF8 = 0; // VBLANK_INTR_WAIT_FLAGS, ARM7 version
	REG_POWERCNT = 1;  // Turn off power to stuff

	REG_AUXIE &= ~(1UL << 8);
	*(vu32*)0x400481C = 0;
	*(vu32*)0x4004820 = 0;

	//driveInitialize();

	aFile file;
	getFileFromCluster(&file, quitFileCluster, (valueBits & quitOnFlashcard));
	if (file.firstCluster == CLUSTER_FREE) {
		// File not found, so reboot console instead
		i2cWriteRegister(0x4A, 0x70, 0x01);
		i2cWriteRegister(0x4A, 0x11, 0x01);
	}

	fileRead((char*)__DSiHeader, &file, 0, sizeof(tDSiHeader));
	*ndsHeader = __DSiHeader->ndshdr;

	fileRead(__DSiHeader->ndshdr.arm9destination, &file, (u32)__DSiHeader->ndshdr.arm9romOffset, __DSiHeader->ndshdr.arm9binarySize);
	fileRead(__DSiHeader->ndshdr.arm7destination, &file, (u32)__DSiHeader->ndshdr.arm7romOffset, __DSiHeader->ndshdr.arm7binarySize);
	if (ndsHeader->unitCode > 0) {
		fileRead(__DSiHeader->arm9idestination, &file, (u32)__DSiHeader->arm9iromOffset, __DSiHeader->arm9ibinarySize);
		fileRead(__DSiHeader->arm7idestination, &file, (u32)__DSiHeader->arm7iromOffset, __DSiHeader->arm7ibinarySize);

		// Disabled due to ce7 code taking place in DSi WRAM
		// initMBK_dsiMode();
	}

	if (!(valueBits & b_dsiSD)) {
		dldiPatchBinary(ndsHeader->arm9destination, ndsHeader->arm9binarySize);
	}

	sharedAddr[0] = 0x44414F4C; // 'LOAD'

	for (i = 0; i < 4; i++) {
		for(reg=0; reg<0x1c; reg+=4)*((vu32*)(0x04004104 + ((i*0x1c)+reg))) = 0;//Reset NDMA.
	}

	while (sharedAddr[0] != 0x544F4F42) { // 'BOOT'
		while (REG_VCOUNT != 191) swiDelay(100);
		while (REG_VCOUNT == 191) swiDelay(100);
	}

	// Start ARM7
	ndsCodeStart(ndsHeader->arm7executeAddress);
#else
	IPC_SendSync(0x8);
	if (consoleModel < 2) {
		unlaunchSetFilename();
		readSoftResetId(true);
		waitFrames(5);							// Wait for DSi screens to stabilize
	} else {
		if (raQuitThroughSync()) {
			*(u32*)0x02000000 &= ~BIT(2);	// BIT(2): "back from a game", no autorun
		}
		readSoftResetId(true);
		waitFrames(1);
	}

	rebootConsole();		// Reboot into TWiLight Menu++
#endif
}

void dumpRam(void) {
	#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
	#endif
	//driveInitialize();
	sharedAddr[3] = 0x444D4152;
	// Dump RAM
	// #ifdef TWLSDK
	fileWrite((char*)0x0C000000, &ramDumpFile, 0, (consoleModel==0 ? 0x01000000 : 0x02000000));
	/* #else
	if (valueBits & dsiMode) {
		// Dump full RAM
		fileWrite((char*)0x0C000000, &ramDumpFile, 0, (consoleModel==0 ? 0x01000000 : 0x02000000));
	} else if (valueBits & isSdk5) {
		// Dump RAM used in DS mode (SDK5)
		fileWrite((char*)0x02000000, &ramDumpFile, 0, 0x3E0000);
		fileWrite((char*)(ndsHeader->unitCode==2 ? 0x02FE0000 : 0x027E0000), &ramDumpFile, 0x3E0000, 0x1F000);
		fileWrite((char*)0x02FFF000, &ramDumpFile, 0x3FF000, 0x1000);
	} else if (moduleParams->sdk_version >= 0x2008000) {
		// Dump RAM used in DS mode (SDK2.1+)
		fileWrite((char*)0x02000000, &ramDumpFile, 0, 0x3E0000);
		fileWrite((char*)0x027E0000, &ramDumpFile, 0x3E0000, 0x20000);
	} else {
		// Dump RAM used in DS mode (SDK2.0)
		fileWrite((char*)0x02000000, &ramDumpFile, 0, 0x3C0000);
		fileWrite((char*)0x027C0000, &ramDumpFile, 0x3C0000, 0x40000);
	}
	#endif */
	sharedAddr[3] = 0;
  	#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
	#endif
}

void prepareScreenshot(void) {
#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
		//driveInitialize();
		fileWrite((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 0x40000);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void saveScreenshot(void) {
	if (igmText->currentScreenshot >= 50) return;

#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
	//driveInitialize();
	fileWrite((char*)INGAME_MENU_EXT_LOCATION, &screenshotFile, 0x200 + (igmText->currentScreenshot * 0x18400), 0x18046);

	// Skip until next blank slot
	char magic;
	do {
		igmText->currentScreenshot++;
		fileRead(&magic, &screenshotFile, 0x200 + (igmText->currentScreenshot * 0x18400), 1);
	} while(magic == 'B' && igmText->currentScreenshot < 50);

		fileRead((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 0x40000);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void prepareManual(void) {
#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
		//driveInitialize();
		fileWrite((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 32 * 24);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void readManual(int line) {
	static int currentManualLine = 0;
	static int currentManualOffset = 0;
	char buffer[32];

	// Seek for desired line
	bool firstLoop = true;
	while(currentManualLine != line) {
		if(line > currentManualLine) {
			fileRead(buffer, &manualFile, currentManualOffset, 32);

			for(int i = 0; i < 32; i++) {
				if(buffer[i] == '\n') {
					currentManualOffset += i + 1;
					currentManualLine++;
					break;
				} else if(i == 31) {
					currentManualOffset += i + 1;
					break;
				}
			}
		} else {
			currentManualOffset -= 32;
			fileRead(buffer, &manualFile, currentManualOffset, 32);
			int i = firstLoop ? 30 : 31;
			firstLoop = false;
			for(; i >= 0; i--) {
				if((buffer[i] == '\n') || currentManualOffset + i == -1) {
					currentManualOffset += i + 1;
					currentManualLine--;
					firstLoop = true;
					break;
				}
			}
		}
	}

	toncset((u8*)INGAME_MENU_EXT_LOCATION, ' ', 32 * 24);
	((vu8*)INGAME_MENU_EXT_LOCATION)[32 * 24] = '\0';

	// Read in 24 lines
	u32 tempManualOffset = currentManualOffset;
	bool fullLine = false;
	for(int line = 0; line < 24 && line < igmText->manualMaxLine; line++) {
		fileRead(buffer, &manualFile, tempManualOffset, 32);

		// Fix for exactly 32 char lines
		if(fullLine && buffer[0] == '\n')
			fileRead(buffer, &manualFile, ++tempManualOffset, 32);

		for(int i = 0; i <= 32; i++) {
			if(i == 32 || buffer[i] == '\n' || buffer[i] == '\0') {
				tempManualOffset += i;
				if(buffer[i] == '\n')
					tempManualOffset++;
				fullLine = i == 32;
				tonccpy((char*)INGAME_MENU_EXT_LOCATION + line * 32, buffer, i);
				break;
			}
		}
	}
}

void restorePreManual(void) {
#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
#endif
		//driveInitialize();
		fileRead((char*)INGAME_MENU_EXT_LOCATION, &pageFile, 0x540000, 32 * 24);
#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
#endif
}

void saveMainScreenSetting(void) {
	fileWrite((char*)&mainScreen, &patchOffsetCacheFile, 0x1FC, sizeof(u32));
}

void saveMainScreenSettingIgm(void) {
	fileWrite((char*)sharedAddr, &patchOffsetCacheFile, 0x1FC, sizeof(u32));
}

static struct RaPopup raPopup;
static struct RaPopup raMenuInfo;
static bool raPopupQueued = false;
static bool raMenuRequested = false;
static const struct RaPopup* raLoadPayload = NULL; // copied in with the menu

// Show a RetroAchievements unlock at the next VBlank that can take it.
void raQueuePopup(const char* title, u32 points) {
	raPopup.magic = RA_POPUP_MAGIC;
	raPopup.points = points;
	int i = 0;
	for (; i < RA_POPUP_TITLE_LEN-1 && title[i]; i++) {
		raPopup.title[i] = title[i];
	}
	raPopup.title[i] = 0;
	raPopupQueued = true;
}

void loadInGameMenu(void) {
	const u32 igmLocation = INGAME_MENU_LOCATION;

	sharedAddr[5] = 0x4C4D4749; // 'IGML'
	fileWrite((char*)igmLocation, &pageFile, 0xA000, 0xA000);	// Backup part of game RAM to page file
	fileRead((char*)igmLocation, &pageFile, 0, 0xA000);	// Read in-game menu
	if (raLoadPayload) {
		// Before ARM9 is let into the menu, so it never sees stale text
		tonccpy((char*)igmLocation + RA_POPUP_OFFSET, raLoadPayload, sizeof(struct RaPopup));
	}
	sharedAddr[5] = 0;
}

void unloadInGameMenu(void) {
	while (REG_VCOUNT != 191) swiDelay(100);
	while (REG_VCOUNT == 191) swiDelay(100);

	const u32 igmLocation = INGAME_MENU_LOCATION;

	sharedAddr[5] = 0x4C4D4749; // 'IGML'
	fileWrite((char*)igmLocation, &pageFile, 0, 0xA000);	// Store in-game menu
	fileRead((char*)igmLocation, &pageFile, 0xA000, 0xA000);	// Restore part of game RAM from page file
	sharedAddr[5] = 0;
}

#ifndef TWLSDK
static void vcountIrqLower()
{
    while (1)
    {
        if (sActiveFpsa.initial)
        {
            sActiveFpsa.initial = FALSE;
            break;
        }

        if (!sActiveFpsa.backJump)
            sActiveFpsa.cycleDelta += sActiveFpsa.targetCycles - ((u64)FPSA_CYCLES_PER_FRAME << 24);
        u32 linesToAdd = 0;
        while (sActiveFpsa.cycleDelta >= (s64)((u64)FPSA_CYCLES_PER_LINE << 23))
        {
            sActiveFpsa.cycleDelta -= (u64)FPSA_CYCLES_PER_LINE << 24;
            if (++linesToAdd == 5)
                break;
        }
        if (linesToAdd == 0)
        {
            sActiveFpsa.backJump = FALSE;
            break;
        }
        if (linesToAdd > 1)
        {
            sActiveFpsa.backJump = TRUE;
        }
        else
        {
            // don't set the backJump flag because the irq is not retriggered if the new vcount
            // is the same as the previous line
            sActiveFpsa.backJump = FALSE;
        }
        // ensure we won't accidentally run out of line time
        while (REG_DISPSTAT & DISP_IN_HBLANK)
            ;
        int curVCount = REG_VCOUNT;
        REG_VCOUNT = curVCount - (linesToAdd - 1);
        if (linesToAdd == 1)
            break;

        while (REG_VCOUNT >= curVCount)//FPSA_ADJUST_MAX_VCOUNT - 5)
            ;
        while (REG_VCOUNT < curVCount)//FPSA_ADJUST_MAX_VCOUNT - 5)
            ;
    }
    REG_IF = IRQ_VCOUNT;
}

static void vcountIrqHigher()
{
    if (sActiveFpsa.initial)
    {
        sActiveFpsa.initial = FALSE;
        return;
    }
    sActiveFpsa.cycleDelta += ((u64)FPSA_CYCLES_PER_FRAME << 24) - sActiveFpsa.targetCycles;
    u32 linesToSkip = 0;
    while (sActiveFpsa.cycleDelta >= (s64)((u64)FPSA_CYCLES_PER_LINE << 23))
    {
        sActiveFpsa.cycleDelta -= (u64)FPSA_CYCLES_PER_LINE << 24;
        if (++linesToSkip == 55)
            break;
    }
    if (linesToSkip == 0)
        return;
    // ensure we won't accidentally run out of line time
    while (REG_DISPSTAT & DISP_IN_HBLANK)
        ;
    REG_VCOUNT = REG_VCOUNT + (linesToSkip + 1);
}

void fpsa_init(fpsa_t* fpsa)
{
    toncset(fpsa, 0, sizeof(fpsa_t));
    fpsa->isStarted = FALSE;
    fpsa_setTargetFrameCycles(fpsa, (u64)FPSA_CYCLES_PER_FRAME << 24); // default to no adjustment
}

void fpsa_start(fpsa_t* fpsa)
{
    // int irq = enterCriticalSection();
    do
    {
        if (fpsa->isStarted)
            break;
        if (fpsa->targetCycles == ((u64)FPSA_CYCLES_PER_FRAME << 24))
            break;
        fpsa->backJump = FALSE;
        fpsa->cycleDelta = 0;
        fpsa->initial = TRUE;
        fpsa->isFpsLower = fpsa->targetCycles >= ((u64)FPSA_CYCLES_PER_FRAME << 24);
        // prevent the irq from immediately happening
        while (REG_VCOUNT != FPSA_ADJUST_MAX_VCOUNT + 2)
            ;
        fpsa->isStarted = TRUE;
        if (fpsa->isFpsLower)
        {
            SetYtrigger(FPSA_ADJUST_MAX_VCOUNT - 5);
        }
        else
        {
            SetYtrigger(FPSA_ADJUST_MIN_VCOUNT);
        }
    } while (0);
    // leaveCriticalSection(irq);
}

void fpsa_stop(fpsa_t* fpsa)
{
    if (!fpsa->isStarted)
        return;
    fpsa->isStarted = FALSE;
}

void fpsa_setTargetFrameCycles(fpsa_t* fpsa, u64 cycles)
{
    fpsa->targetCycles = cycles;
}

void fpsa_setTargetFpsFraction(fpsa_t* fpsa, u32 num, u32 den)
{
    u64 cycles = (((double)FPSA_SYS_CLOCK * den * (1 << 24)) / num) + 0.5;
    fpsa_setTargetFrameCycles(fpsa, cycles);//((((u64)FPSA_SYS_CLOCK * (u64)den) << 24) + ((num + 1) >> 1)) / num);
}

void fpsa_run(void) {
    if (!sActiveFpsa.isStarted) {
        return;
	}
	sActiveFpsa.isFpsLower ? vcountIrqLower() : vcountIrqHigher();
}
#endif

#ifdef DEBUG
static void log_arm9(void) {
	//driveInitialize();
	u32 src = *(vu32*)(sharedAddr+2);
	u32 dst = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\ncard read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nstr : \n");
	dbg_hexa((u32)cardStruct);
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);

	dbg_printf("\nlog only \n");
}
#endif

static void nandRead(void) {
	u32 flash = *(vu32*)(sharedAddr+2);
	u32 memory = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nnand read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nflash : \n");
	dbg_hexa(flash);
	dbg_printf("\nmemory : \n");
	dbg_hexa(memory);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	//driveInitialize();
	//cardReadLED(true, true);    // When a file is loading, turn on LED for card read indicator
	sdmmc_set_ndma_slot(4);
	fileRead((char *)memory, savFile, flash, len);
	sdmmc_set_ndma_slot(0);
	//cardReadLED(false, true);
}

static void nandWrite(void) {
	u32 flash = *(vu32*)(sharedAddr+2);
	u32 memory = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nnand write received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nflash : \n");
	dbg_hexa(flash);
	dbg_printf("\nmemory : \n");
	dbg_hexa(memory);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	//driveInitialize();
	saveTimer = 1;			// When we're saving, power button does nothing, in order to prevent corruption.
	//cardReadLED(true, true);    // When a file is loading, turn on LED for card read indicator
	sdmmc_set_ndma_slot(4);
	fileWrite((char *)memory, savFile, flash, len);
	sdmmc_set_ndma_slot(0);
	//cardReadLED(false, true);
}

#ifdef TWLSDK
static void sharedFontRead(void) {
	u32 flash = *(vu32*)(sharedAddr+2);
	u32 memory = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nshared font read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nflash : \n");
	dbg_hexa(flash);
	dbg_printf("\nmemory : \n");
	dbg_hexa(memory);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	//driveInitialize();
	//cardReadLED(true, false);    // When a file is loading, turn on LED for card read indicator
	sdmmc_set_ndma_slot(4);
	fileRead((char *)memory, sharedFontFile, flash, len);
	sdmmc_set_ndma_slot(0);
	//cardReadLED(false, false);
}
#endif

/*static void slot2Read(void) {
	u32 src = *(vu32*)(sharedAddr+2);
	u32 dst = *(vu32*)(sharedAddr);
	u32 len = *(vu32*)(sharedAddr+1);
	#ifdef DEBUG
	u32 marker = *(vu32*)(sharedAddr+3);

	dbg_printf("\nslot2 read received\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);
	#endif

	cardReadLED(true);    // When a file is loading, turn on LED for card read indicator
	fileRead((char*)dst, *gbaFile, src, len, -1);
	cardReadLED(false);
}*/

static bool readOngoing = false;
//static bool sdReadOngoing = false;
//static bool ongoingIsDma = false;
//static int currentCmd=0, currentNdmaSlot=0;
//static int timeTillDmaLedOff = 0;

static bool start_cardRead_arm9(void) {
	bool useApFixOverlays = false;
	u32 src = sharedAddr[2];
	u32 dst = sharedAddr[0];
	u32 len = sharedAddr[1];
	if (src >= 0x80000000) {
		src -= 0x80000000;
		useApFixOverlays = true;
	}
	#ifdef DEBUG
	u32 marker = sharedAddr[3];

	dbg_printf("\ncard read received v2\n");

	if (calledViaIPC) {
		dbg_printf("\ntriggered via IPC\n");
	}

	dbg_printf("\nstr : \n");
	dbg_hexa((u32)cardStruct);
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	dbg_printf("\nmarker : \n");
	dbg_hexa(marker);	
	#endif

	const bool isDma = (sharedAddr[3] == (vu32)0x025FFB09 || sharedAddr[3] == (vu32)0x025FFB0A);

	//driveInitialize();
	cardReadLED(true, isDma);    // When a file is loading, turn on LED for card read indicator
	#ifdef DEBUG
	nocashMessage("fileRead romFile");
	#endif
	if ((dst % 4) == 0) {
		if(!fileReadNonBLocking((char*)dst, useApFixOverlays ? apFixOverlaysFile : romFile, src, len))
		{
			readOngoing = true;
			return false;
			//while(!resumeFileRead()){}
		} 
		else
		{
			readOngoing = false;
			cardReadLED(false, isDma);    // After loading is done, turn off LED for card read indicator
			return true;
		}
	} else {
		fileRead((char*)dst, useApFixOverlays ? apFixOverlaysFile : romFile, src, len);
		cardReadLED(false, isDma);    // After loading is done, turn off LED for card read indicator
		return true;
	}

	#ifdef DEBUG
	dbg_printf("\nread \n");
	if (is_aligned(dst, 4) || is_aligned(len, 4)) {
		dbg_printf("\n aligned read : \n");
	} else {
		dbg_printf("\n misaligned read : \n");
	}
	#endif
}

static bool resume_cardRead_arm9(void) {
	const bool isDma = (sharedAddr[3] == (vu32)0x025FFB09 || sharedAddr[3] == (vu32)0x025FFB0A);
    if(resumeFileRead())
    {
        readOngoing = false;
        cardReadLED(false, isDma);    // After loading is done, turn off LED for card read indicator
        return true;    
    } 
    else
    {
        return false;    
    }
}

#ifndef TWLSDK
/* static bool gsddFix(void) {
	if (sharedAddr[4] != 0x44445347) {
        return false;
    }

	const u32 gsddOverlayChecksumOffset = *(u32*)0x02FFF004;
	// const u32 gsddOverlayFuncOffset = *(u32*)0x02FFF008;
	const u32 oldChecksum = 0x2FBB82E1;
	const u32 newChecksum = *(u32*)0x02FFF17C;

	if (*(u32*)gsddOverlayChecksumOffset == oldChecksum) {
		*(u32*)gsddOverlayChecksumOffset = newChecksum;
		// *(u32*)gsddOverlayFuncOffset = 0xE1A00000; // nop (Start the game past name setting)
	}

	sharedAddr[4] = 0;
	return true;
} */

/* bool romLocationAdjust(const tNDSHeader* ndsHeader, const bool laterSdk, const bool dsiBios, u32* romLocation) {
	const bool ntrType = (ndsHeader->unitCode == 0);
	const u32 romLocationOld = *romLocation;
	if (*romLocation == 0x0C3FC000) {
		*romLocation += 0x4000;
	} else if (*romLocation == 0x0C7C0000 && ((laterSdk && !dsiBios) || !laterSdk) && ntrType) {
		*romLocation += laterSdk ? 0x8000 : 0x28000;
	} else if (*romLocation == 0x0C7C4000) {
		*romLocation += 0x4000;
	} else if (*romLocation == 0x0C7D8000 && laterSdk) {
		if (ntrType) {
			*romLocation += (valueBits & hasVramWifiBinary) ? 0x10000 : 0x28000;
		} else {
			*romLocation += 0x8000;
		}
	} else if (*romLocation == 0x0C7F8000 && (laterSdk || !dsiBios) && ntrType) {
		*romLocation += 0x8000;
	} else if (*romLocation == 0x0C7FC000) {
		*romLocation += 0x4000;
	} else if (*romLocation == 0x0CFE0000 && !ntrType) {
		*romLocation += 0x20000;
	} else if (*romLocation == 0x0CFFC000 && dsiBios) {
		*romLocation += 0x4000;
	}
	return (*romLocation != romLocationOld);
}

static void loadROMPartIntoRAM(void) {
	static bool finished = false;
	extern u32 romPartLocation;
	extern u32 romPartSrc;
	extern u32 romPartSize;
	extern u32 romPartFrame;

	if (finished || romPartFrame == 0 || *(int*)((valueBits & isSdk5) ? 0x02FFFC3C : 0x027FFC3C) < romPartFrame) {
		return;
	}

	const int oldIME = enterCriticalSection(); // This is needed to avoid crashing when reading or writing save data

	static bool inited = false;
	const u32 cacheBlockSize = 0x4000;

	static s32 preloadSizeEdit = 0;
	static u32 romLocationChange = 0;
	static u32 romOffsetChange = 0;

	if (!inited) {
		preloadSizeEdit = romPartSize;
		romLocationChange = romPartLocation;
		romOffsetChange = romPartSrc;
		inited = true;
	}

	if (preloadSizeEdit > 0) {
		const u32 romBlockSize = (preloadSizeEdit > cacheBlockSize) ? cacheBlockSize : preloadSizeEdit;
		if (lockMutex(&saveMutex)) {
			cardReadLED(true, true);
			fileRead((char*)romLocationChange, romFile, romOffsetChange, romBlockSize);
			cardReadLED(false, true);
			unlockMutex(&saveMutex);
		}
		preloadSizeEdit -= romBlockSize;
		romOffsetChange += cacheBlockSize;
		romLocationChange += cacheBlockSize;

		romLocationAdjust(ndsHeader, !(valueBits & eSdk2), (valueBits & b_dsiBios), &romLocationChange);
	} else {
		sharedAddr[5] = 0x44454C50; // 'PLED'
		finished = true;
	}

	leaveCriticalSection(oldIME);
} */
#endif

#ifdef UNUSED
static inline void sdmmcHandler(void) { // Unused
	if (sdReadOngoing) {
		if (my_sdmmc_sdcard_check_command(0x33C12)) {
			sharedAddr[4] = 0;
			cardReadLED(false, ongoingIsDma);
			sdReadOngoing = false;
		}
		return;
	}

	switch (sharedAddr[4]) {
		case 0x53445231:
		case 0x53444D31: {
		
			//#ifdef DEBUG		
			//dbg_printf("my_sdmmc_sdcard_readsector\n");
			//#endif
			// bool isDma = sharedAddr[4]==0x53444D31;
			// cardReadLED(true, isDma);
			ongoingIsDma = (sharedAddr[4] == 0x53444D31);
			cardReadLED(true, ongoingIsDma);
			sharedAddr[4] = my_sdmmc_sdcard_readsector(sharedAddr[0], (u8*)sharedAddr[1], sharedAddr[2], sharedAddr[3]);
			// cardReadLED(false, isDma);
		}	break;
		case 0x53445244:
		case 0x53444D41: {
		
			//#ifdef DEBUG		
			//dbg_printf("my_sdmmc_sdcard_readsectors\n");
			//#endif
			//bool isDma = sharedAddr[4]==0x53444D41;
			ongoingIsDma = (sharedAddr[4] == 0x53444D41);
			cardReadLED(true, ongoingIsDma);
			if ((sharedAddr[2] % 4) != 0 || (valueBits & ndmaDisabled)) {
				sharedAddr[4] = my_sdmmc_sdcard_readsectors(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
				cardReadLED(false, ongoingIsDma);
			} else {
				my_sdmmc_sdcard_readsectors_nonblocking(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
				sdReadOngoing = true;
			}
		}	break;
		/*case 0x53444348:
			sharedAddr[4] = my_sdmmc_sdcard_check_command(sharedAddr[0], sharedAddr[1]);
			//currentCmd = sharedAddr[0];
			//currentNdmaSlot = sharedAddr[1];
			break;
		case 0x53415244:
			cardReadLED(true, true);
			sharedAddr[4] = my_sdmmc_sdcard_readsectors_nonblocking(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
			//currentCmd = sharedAddr[4];
			//currentNdmaSlot = sharedAddr[3];
			timeTillDmaLedOff = 0;
			readOngoing = true;
			break;*/
		/* case 0x53445752:
			cardReadLED(true, true);
			sharedAddr[4] = my_sdmmc_sdcard_writesectors(sharedAddr[0], sharedAddr[1], (u8*)sharedAddr[2]);
			cardReadLED(false, true);
			break; */
	}
}
#endif

void runCardEngineCheck(void) {
	// if (!(valueBits & b_runCardEngineCheck)) return;

	//dbg_printf("runCardEngineCheck\n");
	#ifdef DEBUG		
	nocashMessage("runCardEngineCheck");
	#endif	

  	// if (tryLockMutex(&cardEgnineCommandMutex)) {
        //if(!readOngoing)
        //{
    
    		//nocashMessage("runCardEngineCheck mutex ok");
    
			if (!(valueBits & gameOnFlashcard)) {
				if (/* sharedAddr[3] == (vu32)0x020FF808 || sharedAddr[3] == (vu32)0x020FF80A || */ sharedAddr[3] >= (vu32)0x025FFB08 && sharedAddr[3] <= (vu32)0x025FFB0A) {	// ARM9 Card Read
					const bool isDma = (sharedAddr[3] == (vu32)0x025FFB0A);
					if (!readOngoing ? start_cardRead_arm9() : resume_cardRead_arm9()) {
						sharedAddr[3] = 0;
					}
					if (isDma) {
						sharedAddr[4] = 0x39414D44; // 'DMA9'
						IPC_SendSync(0x3);
					}
				}
			}

			/* #ifdef DEBUG
    		if (sharedAddr[3] == (vu32)0x026FF800) {
    			log_arm9();
    			sharedAddr[3] = 0;
                //IPC_SendSync(0x8);
    		} else
			#endif

			if (sharedAddr[3] == (vu32)0x025FFC01) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC01);
				nandRead();
    			sharedAddr[3] = 0;
			} else if (sharedAddr[3] == (vu32)0x025FFC02) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC02);
				nandWrite();
    			sharedAddr[3] = 0;
			} */

            /*if (sharedAddr[3] == (vu32)0x025FBC01) {
                dmaLed = false;
    			slot2Read();
    			sharedAddr[3] = 0;
    			IPC_SendSync(0x8);
    		}*/
        //}
  		// unlockMutex(&cardEgnineCommandMutex);
  	// }
}

static void raIdle(void);

void runCardEngineCheckHalt(void) {
	//dbg_printf("runCardEngineCheckHalt\n");
	#ifdef DEBUG		
	nocashMessage("runCardEngineCheckHalt");
	#endif	

  	// if (lockMutex(&cardEgnineCommandMutex)) {
        //if(!readOngoing)
        //{
    
    		//nocashMessage("runCardEngineCheck mutex ok");

			/* #ifndef TWLSDK
			loadROMPartIntoRAM();
			#endif */
			if (/* sharedAddr[3] == (vu32)0x020FF808 || sharedAddr[3] == (vu32)0x020FF80A || */ sharedAddr[3] >= (vu32)0x025FFB08 && sharedAddr[3] <= (vu32)0x025FFB0A) {	// ARM9 Card Read
				const bool isDma = (sharedAddr[3] == (vu32)0x025FFB0A);
				const bool dmaLed = (sharedAddr[3] == (vu32)0x025FFB09 || isDma);
				bool useApFixOverlays = false;
				u32 src = sharedAddr[2];
				u32 dst = sharedAddr[0];
				u32 len = sharedAddr[1];
				if (src >= 0x80000000) {
					src -= 0x80000000;
					useApFixOverlays = true;
				}

				// readOngoing = true;
				if (lockMutex(&saveMutex)) {
					cardReadLED(true, dmaLed);    // When a file is loading, turn on LED for card read indicator
					fileRead((char*)dst, useApFixOverlays ? apFixOverlaysFile : romFile, src, len);
					cardReadLED(false, dmaLed);    // After loading is done, turn off LED for card read indicator
					unlockMutex(&saveMutex);
				}
				// readOngoing = false;
				sharedAddr[3] = 0;
				if (isDma) {
					sharedAddr[4] = 0x39414D44; // 'DMA9'
					IPC_SendSync(0x3);
				}
			}

			#ifdef DEBUG
    		if (sharedAddr[3] == (vu32)0x026FF800) {
    			log_arm9();
    			sharedAddr[3] = 0;
                //IPC_SendSync(0x8);
    		} else
			#endif

			if (sharedAddr[3] == (vu32)0x025FFC01) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC01);
				nandRead();
    			sharedAddr[3] = 0;
			} else if (sharedAddr[3] == (vu32)0x025FFC02) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC02);
				nandWrite();
    			sharedAddr[3] = 0;
			}
			#ifdef TWLSDK
			else if (sharedAddr[3] == (vu32)0x025FFC03) {
				//dmaLed = (sharedAddr[3] == (vu32)0x025FFC03);
				sharedFontRead();
    			sharedAddr[3] = 0;
			}
			#endif

            /*if (sharedAddr[3] == (vu32)0x025FBC01) {
                dmaLed = false;
    			slot2Read();
    			sharedAddr[3] = 0;
    			IPC_SendSync(0x8);
    		}*/
        //}
  		// unlockMutex(&cardEgnineCommandMutex);
  	// }

	// RetroAchievements: evaluate here, in the game's idle time, where
	// interrupts and the game's own threads can preempt it
	raIdle();
}

//---------------------------------------------------------------------------------
void myIrqHandlerFIFO(void) {
//---------------------------------------------------------------------------------
	#ifdef DEBUG		
	nocashMessage("myIrqHandlerFIFO");
	#endif

	// calledViaIPC = true;

    if (IPC_GetSync() == 0x3) {
		/* #ifndef TWLSDK
		if (gsddFix()) {
			return;
		}
		#endif */
		swiDelay(100);
		sharedAddr[4] = 0x39414D44; // 'DMA9'
		IPC_SendSync(0x3);
		return;
	}

	runCardEngineCheck();
	if (!(valueBits & gameOnFlashcard)) {
		// sdmmcHandler();
		if (readOngoing) {
			sharedAddr[5] = 0x474E4950; // 'PING'
		}
	}
}


#ifndef TWLSDK // no room in the DSi-enhanced engines; DS games only for now
// RetroAchievements (see ra_engine.h): load the engine and set staged in
// ramDump.bin by the loader, evaluate every VBlank, record unlocks in a ring
// there and pop them up.  Once a second a probe record with the engine's
// stats goes to another ring, for diagnosis.
#define RA_PROBE_RECORDS 1024 // 64 bytes each, 64KB
#define RA_PENDING_MAX 8
static u32 raFrame = 0;
static u32 raSeq = 0;
static u32 raRecord[16];
// Game RAM words logged with each probe record (RA addresses), for checking
// what the ARM7 sees; set for Tetris DS while bringing the engine up
static const u32 raProbeWatch[6] = {0x076a2c, 0x07b3b0, 0x17dd38, 0x17dd1c, 0x17dd20, 0x17dd30};
static int raState = 0; // 0 = not loaded yet, 1 = running, -1 = off for this game
static u32 raConfig = 0; // RA_CFG_*, from the boot header
static const struct RaNetHeader* raNet; // in-game sending, below
static void raNetLoad(void);
static bool raLedPending; // 3DS LED, below

// On a 3DS the loader pointed TWiLight Menu++'s autorun at RA Sync for a
// game with achievements (ra_boot.cpp raRedirectQuit), but quitting tells
// TWiLight Menu++ not to autorun: returnToLoader() lifts that for these games
static bool raQuitThroughSync(void) {
	return raState == 1;
}

// Hardcore for this game: inGameMenu.c refuses RAM reads/writes/dumps and
// refresh-rate changes
bool raHardcoreActive(void) {
	return (raConfig & RA_CFG_HARDCORE) != 0;
}

// In-game menu (RAHC): softcore/hardcore for this game.  Kept in ramDump.bin
// for the loader's next start.  Leaving hardcore takes effect at once;
// entering it only through that restart (the menu restarts the game), so
// that nothing done before carries over.  Called with saveMutex held.
void raSetHardcore(bool on) {
	static u32 mode[2];
	mode[0] = RA_MODE_MAGIC;
	mode[1] = on;
	fileWrite((char*)mode, &ramDumpFile, RA_DUMP_MODE_OFFSET, sizeof(mode));
	if (!on) {
		raConfig &= ~RA_CFG_HARDCORE;
		((struct RaBootHeader*)RA_REGION)->config = raConfig;
	}
}
static bool raInWram = false; // fast-RAM variant loaded
static struct RaEngineHeader* const raEngine = (struct RaEngineHeader*)(RA_REGION + RA_ENGINE_OFFSET);
static struct RaHost raHost;
static struct RaSignedUnlock raSigned;
static u32 raUnlockSeq = 0;
static u32 raPendingId[RA_PENDING_MAX];
static u32 raPendingPoints[RA_PENDING_MAX];
static u32 raPendingFrame[RA_PENDING_MAX];
static int raPendingCount = 0;
static vu32 raFramesDue = 0;
static u32 raFramesSkipped = 0;

// From raIdle(), so VBlank may interrupt: keep it out of the shared state
static bool raInFrame = false;

// From inside a frame evaluation (engine stack): serve a ROM read the game
// is waiting for now, not after the whole frame.  With the SWI Halt hook in
// place ARM9 doesn't IPC us for reads; it waits for idle time, which is ours.
static void raPoll(void) {
	if (sharedAddr[3] >= (vu32)0x025FFB08 && sharedAddr[3] <= (vu32)0x025FFB0A) {
		runCardEngineCheckHalt(); // raIdle() in there returns: raInFrame
	}
}

static void raUnlocked(u32 achievementId, u32 points, const char* title) {
	const int oldIME = enterCriticalSection();
	if (raPendingCount < RA_PENDING_MAX) {
		raPendingId[raPendingCount] = achievementId;
		raPendingPoints[raPendingCount] = points;
		raPendingFrame[raPendingCount] = raFrame;
		raPendingCount++;
	}
	raQueuePopup(title, points);
	raLedPending = true; // 3DS: the notification LED too
	leaveCriticalSection(oldIME);
}

// Called with saveMutex held and the SD free
static void raLoad(void) {
	struct RaBootHeader* boot = (struct RaBootHeader*)RA_REGION;
	raState = -1;
	fileRead((char*)boot, &ramDumpFile, RA_DUMP_BOOT_OFFSET, RA_ENGINE_OFFSET);
	if (boot->magic != RA_BOOT_MAGIC || boot->engineSize == 0 || boot->engineSize > RA_ENGINE_MAX
	 || boot->setSize == 0 || boot->setSize > RA_SET_MAX) {
		return;
	}
	raConfig = boot->config;
	// The fast-RAM variant needs WRAM-A below the cheat engine, where the
	// game's WiFi binary goes when nds-bootstrap relocates it
	// (and only in the main build: ALTERNATIVE runs where that WRAM isn't
	// available, and 0x037C0000 may then mirror the game's shared WRAM)
	#ifdef ALTERNATIVE
	raInWram = false;
	#else
	raInWram = (raConfig & RA_CFG_WRAM) && !(valueBits & hasVramWifiBinary)
	 && boot->wramMainSize && boot->wramMainSize <= RA_ENGINE_MAX
	 && boot->wramCodeSize && boot->wramCodeSize <= RA_WRAM_SIZE - RA_STACK_SIZE;
	#endif
	if (raInWram) {
		fileRead((char*)raEngine, &ramDumpFile, RA_DUMP_BOOT_OFFSET + RA_STAGE_WRAM_MAIN, boot->wramMainSize);
		fileRead((char*)RA_WRAM_CODE, &ramDumpFile, RA_DUMP_BOOT_OFFSET + RA_STAGE_WRAM_CODE, boot->wramCodeSize);
	} else {
		fileRead((char*)raEngine, &ramDumpFile, RA_DUMP_BOOT_OFFSET + RA_ENGINE_OFFSET, boot->engineSize);
	}
	fileRead((char*)(RA_REGION + RA_SET_OFFSET), &ramDumpFile, RA_DUMP_BOOT_OFFSET + RA_SET_OFFSET, boot->setSize);
	if (raEngine->magic != RA_ENGINE_MAGIC) {
		return;
	}
	raUnlockSeq = boot->unlockSeq;
	raHost.unlocked = raUnlocked;
	raHost.boot = boot;
	raHost.set = (char*)(RA_REGION + RA_SET_OFFSET);
	raHost.setSize = boot->setSize;
	raHost.poll = raPoll;
	if (raEngine->init(&raHost) > 0) {
		raState = 1;
		raNetLoad();
	}
}

// Called with saveMutex held and the SD free
static void raWriteUnlocks(void) {
	struct RaUnlockRecord* const r = &raSigned.record;
	for (int i = 0; i < raPendingCount; i++) {
		toncset(&raSigned, 0, sizeof(raSigned));
		r->magic = RA_UNLOCK_MAGIC;
		r->seq = raUnlockSeq;
		r->achievementId = raPendingId[i];
		r->gameId = raEngine->gameId;
		r->points = raPendingPoints[i];
		r->frame = raPendingFrame[i];
		rtcGetTimeAndDate(r->rtc);
		r->rtc[7] = (raConfig & RA_CFG_HARDCORE) ? 1 : 0; // flags: bit 0 = hardcore
		tonccpy(r->md5, raEngine->md5, sizeof(r->md5));
		// Signed with the console key: records added or edited on the SD
		// card don't verify and are never sent
		raEngine->sign(r, sizeof(*r), raSigned.mac);
		fileWrite((char*)&raSigned, &ramDumpFile, RA_DUMP_UNLOCK_OFFSET + (raUnlockSeq % RA_UNLOCK_RECORDS) * sizeof(raSigned), sizeof(raSigned));
		if (raNet) {
			raNet->award(r->seq, r->achievementId, 0, 0); // softcore, as RA Sync sends them
		}
		raUnlockSeq++;
	}
	raPendingCount = 0;
}

static void raWriteProbe(void) {
	const struct RaStats* stats = (raState == 1) ? raEngine->stats : NULL;
	raRecord[0] = 0x33544152; // 'RAT3'
	raRecord[1] = raSeq;
	raRecord[2] = raFrame;
	raRecord[3] = REG_KEYINPUT | (raState << 16);
	raRecord[4] = stats ? (stats->achievements | (stats->parsed << 16)) : 0;
	raRecord[5] = stats ? (stats->parseErrors | (stats->unlocks << 16)) : 0;
	raRecord[6] = stats ? (stats->lastLines | (stats->maxLines << 16)) : 0;
	raRecord[7] = stats ? (stats->frames | (raFramesSkipped << 16)) : 0;
	raRecord[8] = raConfig | (raInWram ? 0x80000000 : 0);
	raRecord[9] = raEngine->magic;
	for (int i = 0; i < 6; i++) {
		raRecord[10+i] = *(vu32*)(0x02000000 + raProbeWatch[i]);
	}
	fileWrite((char*)raRecord, &ramDumpFile, RA_DUMP_PROBE_OFFSET + (raSeq % RA_PROBE_RECORDS) * sizeof(raRecord), sizeof(raRecord));
	raSeq++;
}

// ---------------------------------------------------------------------------
// In-game sending (ra-nds' ranet.bin, ra_netblob.h): staged by the loader
// with RA Sync's network profile and TLS session (RA_CFG_NET), loaded with
// the engine, woken by the first unlock and run in the game's idle time.
// What it can't send stays in the ring for RA Sync as before; RA answers
// "already unlocked" to what RA Sync then sends again.
// ---------------------------------------------------------------------------
#define RA_NETLOG_MAGIC 0x4C4E4152 // 'RANL': { magic, length, text }
static struct RaNetHost raNetHost;
static struct RaNetConfig raNetConfig;
static bool raNetLogDirty = false;
#define RA_NET_SENT_QUEUE 8
static struct RaNetSent raNetSentQueue[RA_NET_SENT_QUEUE];
static volatile int raNetSentCount = 0;
static volatile int raNetEnableReq = 0; // in-game menu: 0 nothing, 1 off, 2 on

// 33.5 MHz / 64 from the frame count and scanline: the game owns the
// timers.  raFrame counts at VBlank (line 192), so the line counts from
// there too, else the time would step back once a frame.
static u32 raNetTicks(void) {
	u32 frame, line;
	do {
		frame = raFrame;
		line = REG_VCOUNT;
	} while (frame != raFrame);
	const u32 sinceVBlank = (line >= 192) ? line - 192 : line + 263 - 192;
	return frame * 8753 + sinceVBlank * 33;
}

static u32 raNetEntropy(void) {
	return REG_VCOUNT ^ (raFrame << 9) ^ TIMER_DATA(0) ^ (TIMER_DATA(1) << 16) ^ REG_KEYINPUT;
}

static u8 raNetI2cRead(u8 dev, u8 reg) {
	const int oldIME = enterCriticalSection();
	const u8 value = i2cReadRegister(dev, reg);
	leaveCriticalSection(oldIME);
	return value;
}

static int raNetI2cWrite(u8 dev, u8 reg, u8 data) {
	const int oldIME = enterCriticalSection();
	i2cWriteRegister(dev, reg, data);
	leaveCriticalSection(oldIME);
	return 1;
}

// Firmware flash (the WiFi settings), at most a slot at a time; interrupts
// off so the game's own SPI use (touch screen) can't cut in
static int raNetNvramRead(void* dst, u32 addr, u32 len) {
	u8* out = (u8*)dst;
	const int oldIME = enterCriticalSection();
	while (REG_SPICNT & SPI_BUSY);
	REG_SPICNT = SPI_ENABLE | SPI_BYTE_MODE | SPI_CONTINUOUS | SPI_DEVICE_NVRAM;
	const u8 cmd[4] = {0x03, addr >> 16, addr >> 8, addr}; // READ
	for (int i = 0; i < 4; i++) {
		REG_SPIDATA = cmd[i];
		while (REG_SPICNT & SPI_BUSY);
	}
	for (u32 i = 0; i < len; i++) {
		REG_SPIDATA = 0;
		while (REG_SPICNT & SPI_BUSY);
		out[i] = REG_SPIDATA;
	}
	REG_SPICNT = 0;
	leaveCriticalSection(oldIME);
	return 1;
}

// The blob's log: kept here, written to ramDump.bin now and then (VBlank),
// moved to sd:/_nds/ra/ranet_log.txt by the loader at the next start
static void raNetLog(const char* text, u32 len) {
	u32* head = (u32*)RA_NET_LOG_RAM;
	char* buf = (char*)(head + 2);
	for (u32 i = 0; i < len && head[1] < RA_NETLOG_SIZE - 8; i++) {
		buf[head[1]++] = text[i];
	}
	raNetLogDirty = true;
}

// With the engine (raLoad): SD free, saveMutex held
static void raNetLoad(void) {
	if (!(raConfig & RA_CFG_NET) || (valueBits & hasVramWifiBinary)) {
		return; // off, or the game uses DS WiFi
	}
	struct RaNetStage* stage = (struct RaNetStage*)RA_NET_STAGE_TEMP;
	fileRead((char*)stage, &ramDumpFile, RA_DUMP_BOOT_OFFSET + RA_STAGE_NET_DATA, sizeof(*stage));
	if (stage->magic != RA_NET_STAGE_MAGIC || stage->blobSize == 0 || stage->blobSize > RA_NET_BLOB_MAX) {
		return;
	}
	fileRead((char*)RA_NET_BLOB_ADDRESS, &ramDumpFile, RA_DUMP_BOOT_OFFSET + RA_STAGE_NET_BLOB, stage->blobSize);
	const struct RaNetHeader* h = (const struct RaNetHeader*)RA_NET_BLOB_ADDRESS;
	u32* log = (u32*)RA_NET_LOG_RAM;
	log[0] = RA_NETLOG_MAGIC;
	log[1] = 0;
	if (h->magic == RA_NET_BLOB_MAGIC && h->version == RA_NET_BLOB_VERSION
	 && h->imageEnd - RA_NET_BLOB_ADDRESS == stage->blobSize && h->bssEnd <= RA_NET_LOG_RAM) {
		raNetHost.size = sizeof(raNetHost);
		raNetHost.ticks = raNetTicks;
		raNetHost.entropy = raNetEntropy;
		raNetHost.i2cRead = raNetI2cRead;
		raNetHost.i2cWrite = raNetI2cWrite;
		raNetHost.nvramRead = raNetNvramRead;
		raNetHost.log = raNetLog;
		raNetConfig.size = sizeof(raNetConfig);
		raNetConfig.profile = &stage->profile;
		raNetConfig.tls = &stage->tls;
		tonccpy(raNetConfig.user, stage->user, sizeof(raNetConfig.user));
		tonccpy(raNetConfig.token, stage->token, sizeof(raNetConfig.token));
		tonccpy(raNetConfig.md5, raEngine->md5, 32);
		raNetConfig.md5[32] = 0;
		raNetConfig.gameId = raEngine->gameId;
		if (h->init(&raNetHost, &raNetConfig)) {
			raNet = h;
			raNetLog("[ce] ranet loaded\n", 18);
			if (raConfig & RA_CFG_NET_OFF) {
				raNet->enable(0); // switched off for this game (in-game menu)
			}
		} else {
			raNetLog("[ce] ranet init failed\n", 23);
		}
	}
	// The blob keeps its own copy: no token or session secret left here
	toncset(stage, 0, sizeof(*stage));
	toncset(raNetConfig.token, 0, sizeof(raNetConfig.token));
}

// Game idle time, before the frame evaluation (raIdle).  Never re-entered:
// an interrupt during the poll may switch to another of the game's ARM7
// threads (the SDK's), which then halts and gets here again.
static void raNetIdle(void) {
	static bool raNetBusy = false;
	if (!raNet || raNetBusy || raInFrame || sharedAddr[3] != 0) {
		return;
	}
	raNetBusy = true;
	if (raNetEnableReq) {
		raNet->enable(raNetEnableReq == 2);
		raNetEnableReq = 0;
	}
	raNet->poll();
	// The account's unlocks, once connected: earned on another console,
	// they are done here too (no popup again; the engine only raises
	// locked ones).  Same idle context as the engine's frames.
	const u32* ids;
	u32 idCount;
	if (raNet->accountUnlocks(&ids, &idCount)) {
		struct RaAchievement* list = raEngine->achievements;
		for (u32 i = 0; i < raEngine->count; i++) {
			for (u32 j = 0; j < idCount && list[i].status == RA_LOCKED; j++) {
				if (list[i].id == ids[j]) {
					list[i].status = RA_UNLOCKED_BEFORE;
				}
			}
		}
	}
	u32 seq, result;
	while (raNet->result(&seq, &result)) {
		// Sent ones are written to ramDump.bin (VBlank) for RA Sync to skip
		const int oldIME = enterCriticalSection();
		if (result == RA_AWARD_SENT && raNetSentCount < RA_NET_SENT_QUEUE) {
			raNetSentQueue[raNetSentCount].seq = seq;
			raNetSentQueue[raNetSentCount].result = result;
			raNetSentCount++;
		}
		leaveCriticalSection(oldIME);
	}
	raNetBusy = false;
}

// In-game menu (RART): real-time upload on/off for this game.  Called with
// saveMutex held.  The blob is told at the next idle poll (not from here:
// the menu may have interrupted a poll); the loader remembers the choice
// for the game (RaRealtimeChoice).
void raSetRealtime(bool on) {
	raConfig = on ? (raConfig & ~RA_CFG_NET_OFF) : (raConfig | RA_CFG_NET_OFF);
	((struct RaBootHeader*)RA_REGION)->config = raConfig;
	raNetEnableReq = on ? 2 : 1;
	static struct RaRealtimeChoice choice;
	choice.magic = RA_REALTIME_MAGIC;
	choice.on = on;
	tonccpy(choice.md5, raEngine->md5, sizeof(choice.md5));
	fileWrite((char*)&choice, &ramDumpFile, RA_DUMP_REALTIME_OFFSET, sizeof(choice));
}

// VBlank, SD free, saveMutex held
static void raNetWriteSent(void) {
	for (int i = 0; i < raNetSentCount; i++) {
		fileWrite((char*)&raNetSentQueue[i], &ramDumpFile,
			RA_DUMP_SENT_OFFSET + (raNetSentQueue[i].seq % RA_UNLOCK_RECORDS) * sizeof(struct RaNetSent),
			sizeof(struct RaNetSent));
	}
	raNetSentCount = 0;
}

// ---------------------------------------------------------------------------
// 3DS: the notification LED on unlocks, through TWPatch's TwlBg.  RTCom:
// the ARM7 and TwlBg's ARM11 talk through two RTC registers, the ARM11
// answering with the SIO interrupt flag.  Our ARM11 program (arm11_ra,
// ra_uc11.h) is uploaded once, 3 s into the game, unless TwlBg still has
// it from the last game; a TwlBg without RTCom doesn't answer the first
// ping and nothing more is tried.
// ---------------------------------------------------------------------------
#include "ra_ucode.h"
#include "ra_uc11.h"
#define RA_RTC_CR      (*(vu8*)0x04000138)
#define RA_RCNT        (*(vu16*)0x04000134)
#define RTCOM_READY    0x00
#define RTCOM_ACK      0x80
#define RTCOM_DONE     0x82
#define RTCOM_UPLOAD   0x41
#define RTCOM_FINISH   0x42
#define RTCOM_EXECUTE  0x44
#define RTCOM_NEXT     0x81
#define RTCOM_KILL     0xFE
static int raUcState = 0; // 0 not tried, 1 ready, -1 unavailable
static bool raLedPending = false;
static u16 raUcRcnt;

static void rtcomDelay(int n) {
	for (vu32 i = n; i; i--);
}

// One RTC transfer, bits high first: the command byte, then a byte out
// (out >= 0) or in (returned)
static int rtcomXfer(u8 cmd, int out) {
	const int oldIME = enterCriticalSection();
	RA_RTC_CR = (1<<6) | (1<<5)|(1<<1) | (1<<4)|1;          // CS low, SCK high
	rtcomDelay(2);
	RA_RTC_CR = (1<<6)|(1<<2) | (1<<5)|(1<<1) | (1<<4)|1;   // CS high
	rtcomDelay(2);
	u32 bits = (out >= 0) ? ((u32)cmd << 8 | (u8)out) : cmd;
	const int n = (out >= 0) ? 16 : 8;
	for (int b = n - 1; b >= 0; b--) {
		const u8 sio = (1<<4) | ((bits >> b) & 1);
		RA_RTC_CR = (1<<6)|(1<<2) | (1<<5) | sio;            // SCK low
		rtcomDelay(9);
		RA_RTC_CR = (1<<6)|(1<<2) | (1<<5)|(1<<1) | sio;     // SCK high
		rtcomDelay(9);
	}
	int in = 0;
	if (out < 0) {
		for (int b = 0; b < 8; b++) {
			RA_RTC_CR = (1<<6)|(1<<2) | (1<<5);
			rtcomDelay(9);
			RA_RTC_CR = (1<<6)|(1<<2) | (1<<5)|(1<<1);
			rtcomDelay(9);
			in = (in << 1) | (RA_RTC_CR & 1);
		}
	}
	rtcomDelay(2);
	RA_RTC_CR = (1<<6) | (1<<5)|(1<<1);                      // CS low
	rtcomDelay(2);
	leaveCriticalSection(oldIME);
	return in;
}

#define rtcomData()        rtcomXfer(0x6D, -1)       // register "112"
#define rtcomSetParam(v)   rtcomXfer(0x6C, (v))
#define rtcomStatus()      rtcomXfer(0x6F, -1)       // register "113"
#define rtcomSetRequest(v) rtcomXfer(0x6E, (v))

static bool rtcomWait(u8 status, int timeout) {
	do {
		if (REG_IF & IRQ_NETWORK) {
			REG_IF = IRQ_NETWORK;
			return rtcomStatus() == status;
		}
	} while (--timeout);
	REG_IF = IRQ_NETWORK;
	return false;
}

static bool rtcomRequest(u8 request, u8 param) {
	rtcomSetParam(param);
	rtcomSetRequest(request);
	return rtcomWait(RTCOM_ACK, 1000000);
}

static void rtcomBegin(void) {
	raUcRcnt = RA_RCNT;
	REG_IF = IRQ_NETWORK;
	RA_RCNT = 0x8100; // the ARM11's answers raise the SIO interrupt flag
	REG_IF = IRQ_NETWORK;
}

static void rtcomEnd(void) {
	rtcomSetRequest(RTCOM_KILL);
	rtcomWait(RTCOM_READY, 1000000);
	rtcomSetRequest(RTCOM_DONE);
	REG_IF = IRQ_NETWORK;
	RA_RCNT = raUcRcnt;
}

// Runs a command of our ARM11 program; its answer, or -1
static int raUcCall(u8 command) {
	return rtcomRequest(RTCOM_EXECUTE, command) ? rtcomData() : -1;
}

static void raUcSetUp(void) {
	rtcomBegin();
	// Anyone there?  (TwlBg without RTCom: no answer, short wait)
	bool answered = false;
	for (int i = 0; i < 3 && !answered; i++) {
		rtcomSetRequest(1);
		answered = rtcomWait(RTCOM_DONE, 200000);
	}
	int ready = 0;
	if (answered) {
		rtcomSetRequest(RTCOM_KILL);
		rtcomWait(RTCOM_READY, 1000000);
		if (raUcCall(RA_UC_HELLO) == RA_UC_MAGIC) {
			ready = 1; // still there from the last game
		} else {
			rtcomSetRequest(RTCOM_KILL);
			rtcomWait(RTCOM_READY, 1000000);
			const u32 len = sizeof(raUc11);
			bool ok = rtcomRequest(RTCOM_UPLOAD, len & 0xFF)
			       && rtcomRequest(RTCOM_NEXT, (len >> 8) & 0xFF)
			       && rtcomRequest(RTCOM_NEXT, (len >> 16) & 0xFF)
			       && rtcomRequest(RTCOM_NEXT, (len >> 24) & 0xFF);
			for (u32 i = 0; ok && i < len; i++) {
				ok = rtcomRequest(RTCOM_NEXT, raUc11[i]);
			}
			ok = ok && rtcomRequest(RTCOM_FINISH, 0);
			if (ok) {
				rtcomSetRequest(RTCOM_KILL);
				rtcomWait(RTCOM_READY, 1000000);
				ready = raUcCall(RA_UC_HELLO) == RA_UC_MAGIC;
			}
		}
	}
	rtcomEnd();
	raUcState = ready ? 1 : -1;
	if (raNet) {
		raNetLog(ready ? "[ce] 3DS LED ready\n" : answered ? "[ce] 3DS LED: upload failed\n" : "[ce] 3DS LED: no RTCom\n",
			ready ? 19 : answered ? 28 : 23);
	}
}

// Game idle time (raIdle), 3DS only
static void raUcIdle(void) {
	static bool busy = false;
	if (consoleModel < 2 || raUcState < 0 || busy || raInFrame || raState != 1 || sharedAddr[3] != 0) {
		return;
	}
	busy = true;
	if (raUcState == 0) {
		if (raFrame > 60*3) {
			raUcSetUp();
		}
	} else if (raLedPending) {
		raLedPending = false;
		rtcomBegin();
		raUcCall(RA_UC_LED);
		rtcomEnd();
	}
	busy = false;
}

static int raComboFrames = 0;

// Game idle time (SWI Halt hook): one evaluation per VBlank at most, and
// none while ARM9 waits for a ROM read
static void raIdle(void) {
	raNetIdle();
	raUcIdle();
	if (raInFrame || raState != 1 || raFramesDue == 0 || sharedAddr[3] != 0) {
		return;
	}
	const int oldIME = enterCriticalSection();
	raFramesSkipped += raFramesDue - 1;
	raFramesDue = 0;
	leaveCriticalSection(oldIME);
	// ARM9 main-memory priority just for the evaluation (after the SDK's
	// start-up IPC handshake), so the game's own ARM7 code, sound included,
	// keeps its usual priority the rest of the frame
	const bool prio = ((raConfig & RA_CFG_PRIO_MASK) >> RA_CFG_PRIO_SHIFT) == RA_PRIO_DURING && raFrame > 60*5;
	if (prio) {
		IPC_SendSync(0xB);
	}
	raInFrame = true;
	raEngine->frame();
	raInFrame = false;
	if (prio) {
		IPC_SendSync(0xC);
	}
}

static void raVBlank(void) {
	raFrame++;
	const u32 interval = raConfig >> RA_CFG_INTERVAL_SHIFT & 0xFF;
	if (raState == 1 && (interval <= 1 || raFrame % interval == 0)) {
		raFramesDue++;
		// The game's ARM9 data cache is write-back; asking it to clean with
		// IPC sync 0xB every frame from the start hung Tetris DS on black
		// screens (the SDK's own IPC sync handshake?), so off while testing
		// what the ARM7 sees without it.
		#ifdef RA_ARM9_CACHE_CLEAN
		IPC_SendSync(0xB);
		#endif
		// Once a second, clear of the SDK's start-up IPC handshake: ask the
		// ARM9 to take main-memory priority (and keep it if the game resets it)
		if (((raConfig & RA_CFG_PRIO_MASK) >> RA_CFG_PRIO_SHIFT) == RA_PRIO_ALWAYS
		 && raFrame > 60*5 && raFrame % 60 == 30) {
			IPC_SendSync(0xB);
		}
	}

	// Achievements list: Select+Down held for half a second, without L so
	// the in-game menu's L+Down+Select still gets through.  Only for games
	// with achievements: every game runs this build, and some use the combo.
	const u16 keys = REG_KEYINPUT;
	if (raState == 1 && !(keys & (KEY_SELECT | KEY_DOWN)) && (keys & KEY_L)) {
		if (++raComboFrames == 30) {
			raMenuInfo.points = 1; // loaded
			raMenuRequested = true;
		}
	} else {
		raComboFrames = 0;
	}

	// Diagnostics only while the engine runs, not for every game
	const bool probeDue = (raState == 1 && raFrame % 60 == 0);
	const bool netLogDue = (raNetLogDirty && raFrame % 60 == 37); // at most once a second
	if (!driveInited || readOngoing || !(raState == 0 || raPendingCount > 0 || probeDue || netLogDue || raNetSentCount > 0)) {
		return;
	}
	if (!(valueBits & bootstrapOnFlashcard) && isSdEjected()) {
		return;
	}
	// Never spin here: the holder may be the code this IRQ interrupted.
	if (!tryLockMutex(&saveMutex)) {
		return;
	}
	sdmmc_set_ndma_slot(4);
	if (raState == 0) {
		raLoad();
	}
	if (raPendingCount > 0) {
		raWriteUnlocks();
	}
	if (probeDue) {
		raWriteProbe();
	}
	if (raNetSentCount > 0) {
		raNetWriteSent();
	}
	if (netLogDue) {
		raNetLogDirty = false;
		const u32 len = ((u32*)RA_NET_LOG_RAM)[1];
		fileWrite((char*)RA_NET_LOG_RAM, &ramDumpFile, RA_DUMP_NETLOG_OFFSET, (8 + len + 3) & ~3);
	}
	sdmmc_set_ndma_slot(0);
	unlockMutex(&saveMutex);
}
#else
static inline void raVBlank(void) {}
static inline void raIdle(void) {}
#endif

void myIrqHandlerVBlank(void) {
  while (1) {
	#ifdef DEBUG		
	nocashMessage("myIrqHandlerVBlank");
	#endif	

	if (valueBits & i2cBricked) {
		REG_MASTER_VOLUME = noI2CVolLevel;
	}

	#ifdef DEBUG
	nocashMessage("cheat_engine_start\n");
	#endif

	if (*(u32*)cheatEngineAddr == 0x3E4 && *(u32*)(cheatEngineAddr+0x3E8) != 0xCF000000) {
		volatile void (*cheatEngine)() = (volatile void*)cheatEngineAddr+4;
		(*cheatEngine)();
	}

	raVBlank();

	if (language >= 0 && language <= 7 && languageTimer < 60*3) {
		// Change language
		personalData->language = language;
		#ifndef TWLSDK
		if (languageAddr > 0) {
			// Extra measure for specific games
			*languageAddr = language;
		}
		#endif
		languageTimer++;
	}

	if (!funcsUnpatched && *(int*)((valueBits & isSdk5) ? 0x02FFFC3C : 0x027FFC3C) >= 60) {
		unpatchedFunctions* unpatchedFuncs = (unpatchedFunctions*)((valueBits & isSdk5) ? UNPATCHED_FUNCTION_LOCATION_SDK5 : UNPATCHED_FUNCTION_LOCATION);

		if (unpatchedFuncs->compressed_static_end) {
			*unpatchedFuncs->compressedFlagOffset = unpatchedFuncs->compressed_static_end;
		}

		#ifdef TWLSDK
		if (unpatchedFuncs->ltd_compressed_static_end) {
			*unpatchedFuncs->iCompressedFlagOffset = unpatchedFuncs->ltd_compressed_static_end;
		}

		if (unpatchedFuncs->mpuInitOffset2) {
			*unpatchedFuncs->mpuInitOffset2 = 0xEE060F12;
		}
		if (unpatchedFuncs->mpuDataOffset2) {
			*unpatchedFuncs->mpuDataOffset2 = unpatchedFuncs->mpuInitRegionOldData2;
		}
		#else
		if (!(valueBits & isSdk5)) {
			if (unpatchedFuncs->mpuDataOffset) {
				*unpatchedFuncs->mpuDataOffset = unpatchedFuncs->mpuInitRegionOldData;

				if (unpatchedFuncs->mpuAccessOffset) {
					if (unpatchedFuncs->mpuOldInstrAccess) {
						unpatchedFuncs->mpuDataOffset[unpatchedFuncs->mpuAccessOffset] = unpatchedFuncs->mpuOldInstrAccess;
					}
					if (unpatchedFuncs->mpuOldDataAccess) {
						unpatchedFuncs->mpuDataOffset[unpatchedFuncs->mpuAccessOffset + 1] = unpatchedFuncs->mpuOldDataAccess;
					}
				}
			}

			if ((u32)unpatchedFuncs->mpuDataOffsetAlt >= (u32)ndsHeader->arm9destination && (u32)unpatchedFuncs->mpuDataOffsetAlt < (u32)ndsHeader->arm9destination+0x4000) {
				*unpatchedFuncs->mpuDataOffsetAlt = unpatchedFuncs->mpuInitRegionOldDataAlt;
			}

			if (unpatchedFuncs->mpuDataOffset2) {
				*unpatchedFuncs->mpuDataOffset2 = unpatchedFuncs->mpuInitRegionOldData2;
			}
		}

		if (unpatchedFuncs->mpuInitOffset2) {
			*unpatchedFuncs->mpuInitOffset2 = 0xEE060F12;
		}
		#endif

		funcsUnpatched = true;
	}

#ifndef TWLSDK
	if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM) && isSdEjected()) {
		tonccpy((u32*)0x02000300, sr_data_error, 0x020);
		rebootConsole();		// Reboot into error screen if SD card is removed
	}
#endif

/* #ifndef TWLSDK
	if (valueBits & isDlp) {
		if (!(REG_EXTKEYINPUT & KEY_A) && *(u32*)(NDS_HEADER_SDK5+0xC) != 0 && !wifiIrq) {
			IPC_SendSync(0x5);
			reset(false);
		}
	}
#endif */

	if ((0 == (REG_KEYINPUT & igmHotkey) && 0 == (REG_EXTKEYINPUT & (((igmHotkey >> 10) & 3) | ((igmHotkey >> 6) & 0xC0))) && (valueBits & igmAccessible) && !wifiIrq) /* || returnToMenu */ || sharedAddr[5] == 0x4C4D4749 /* IGML */) {
		if (tryLockMutex(&saveMutex)) {
#ifdef TWLSDK
		igmText = (struct IgmText *)INGAME_MENU_LOCATION;
		i2cWriteRegister(0x4A, 0x12, 0x00);
#endif
		inGameMenu(0x554E454D); // 'MENU'
#ifdef TWLSDK
		i2cWriteRegister(0x4A, 0x12, 0x01);
#endif
		unlockMutex(&saveMutex);
		}
	} else if (raPopupQueued && (valueBits & igmAccessible) && !wifiIrq && !readOngoing) {
		if (tryLockMutex(&saveMutex)) {
#ifdef TWLSDK
		igmText = (struct IgmText *)INGAME_MENU_LOCATION;
		i2cWriteRegister(0x4A, 0x12, 0x00);
#endif
		raPopupQueued = false;
		raLoadPayload = &raPopup;
		inGameMenu(RA_POPUP_MAGIC);
		raLoadPayload = NULL;
#ifdef TWLSDK
		i2cWriteRegister(0x4A, 0x12, 0x01);
#endif
		unlockMutex(&saveMutex);
		}
	} else if (raMenuRequested && (valueBits & igmAccessible) && !wifiIrq && !readOngoing) {
		if (tryLockMutex(&saveMutex)) {
#ifdef TWLSDK
		igmText = (struct IgmText *)INGAME_MENU_LOCATION;
		i2cWriteRegister(0x4A, 0x12, 0x00);
#endif
		raMenuRequested = false;
		raMenuInfo.magic = RA_MENU_MAGIC;
		raLoadPayload = &raMenuInfo;
		inGameMenu(RA_MENU_MAGIC);
		raLoadPayload = NULL;
#ifdef TWLSDK
		i2cWriteRegister(0x4A, 0x12, 0x01);
#endif
		unlockMutex(&saveMutex);
		}
	}

	if (afterSwapTimer > 0) {
		if (afterSwapTimer == 60*3) {
			if (lockMutex(&saveMutex)) {
				saveMainScreenSetting();
			}
			unlockMutex(&saveMutex);
			afterSwapTimer = 0;
		} else afterSwapTimer++;
	}

	u8 screenIpc = 0x6;

	if (0 == (REG_KEYINPUT & screenSwapHotkey) && 0 == (REG_EXTKEYINPUT & (((screenSwapHotkey >> 10) & 3) | ((screenSwapHotkey >> 6) & 0xC0)))) {
		if (swapTimer == 60){
			swapTimer = 0;
			screenIpc = 0x7;
			mainScreen++;
			if (mainScreen > 2) {
				mainScreen = 0;
			}
			afterSwapTimer = 1;
		}
		swapTimer++;
	} else {
		swapTimer = 0;
	}

#ifdef TWLSDK
	if (sharedAddr[3] == (vu32)0x54495845) {
		returnToLoader(false);
	}
#endif

	if (0 == (REG_KEYINPUT & (KEY_L | KEY_R | KEY_DOWN | KEY_B))) {
		if (returnTimer == 60 * 2) {
#ifdef TWLSDK
			IPC_SendSync(0x5);
#else
			returnToLoader(false);
#endif
		}
		returnTimer++;
	} else {
		returnTimer = 0;
	}

	/* if ((valueBits & b_dsiSD) && (0 == (REG_KEYINPUT & (KEY_L | KEY_R | KEY_DOWN | KEY_A)))) {
		if (tryLockMutex(&cardEgnineCommandMutex)) {
			if (ramDumpTimer == 60 * 2) {
				REG_MASTER_VOLUME = 0;
				int oldIME = enterCriticalSection();
				dumpRam();
				leaveCriticalSection(oldIME);
				REG_MASTER_VOLUME = 127;
			}
			unlockMutex(&cardEgnineCommandMutex);
		}
		ramDumpTimer++;
	} else {
		ramDumpTimer = 0;
	} */

	if (sharedAddr[3] == (vu32)0x52534554) {
		reset(false);
	}

	if ( 0 == (REG_KEYINPUT & (KEY_L | KEY_R | KEY_START | KEY_SELECT))) {
		if (softResetTimer == 60 * 2) {
			if (saveTimer == 0) {
				if (lockMutex(&saveMutex)) {
					REG_MASTER_VOLUME = 0;
					int oldIME = enterCriticalSection();
					forceGameReboot();
					leaveCriticalSection(oldIME);
				}
				unlockMutex(&saveMutex);
			}
		} else {
			softResetTimer++;
		}
	} else {
		softResetTimer = 0;
	}

	#ifndef TWLSDK
	if (valueBits & powerCodeOnVBlank) {
		i2cIRQHandler();
	}
	#endif

	if ((valueBits & preciseVolumeControl) || (valueBits & i2cBricked)) {
		// Precise volume adjustment (for DSi)
		if (volumeAdjustActivated) {
			volumeAdjustDelay++;
			if (volumeAdjustDelay == 30) {
				volumeAdjustDelay = 0;
				volumeAdjustActivated = false;
			}
		} else if (0==(REG_KEYINPUT & KEY_SELECT)) {
			if (valueBits & i2cBricked) {
				const int oldVolLevel = noI2CVolLevel;
				if (0==(REG_KEYINPUT & KEY_UP)) {
					noI2CVolLevel = 127;
				} else if (0==(REG_KEYINPUT & KEY_DOWN)) {
					noI2CVolLevel = 0;
				}
				volumeAdjustActivated = (noI2CVolLevel != oldVolLevel);
			} else {
				const u8 i2cVolLevel = i2cReadRegister(0x4A, 0x40);
				u8 i2cNewVolLevel = i2cVolLevel;
				if (0==(REG_KEYINPUT & KEY_UP)) {
					i2cNewVolLevel++;
				} else if (0==(REG_KEYINPUT & KEY_DOWN)) {
					i2cNewVolLevel--;
				}
				if (i2cNewVolLevel == 0xFF) {
					i2cNewVolLevel = 0;
				} else if (i2cNewVolLevel > 0x1F) {
					i2cNewVolLevel = 0x1F;
				}
				if (i2cNewVolLevel != i2cVolLevel) {
					i2cWriteRegister(0x4A, 0x40, i2cNewVolLevel);
					volumeAdjustActivated = true;
				}
			}
		}
	}
	
	if (saveTimer > 0) {
		saveTimer++;
		if (saveTimer == 60) {
			//i2cWriteRegister(0x4A, 0x12, 0x00);		// If saved, power button works again.
			saveTimer = 0;
		}
	}

	if (REG_IE & IRQ_NETWORK) {
		REG_IE &= ~IRQ_NETWORK; // DSi RTC fix
	}

	bool wifiIrqCheck = (REG_WIFIIRQ != 0);
	if (wifiIrq != wifiIrqCheck) {
		// Turn off card read DMA if WiFi is used, and back on when not in use
		if (wifiIrq) {
			wifiIrqTimer++;
			if (wifiIrqTimer == 30) {
				// IPC_SendSync(0x4);
				wifiIrq = wifiIrqCheck;
			}
		} else {
			// IPC_SendSync(0x4);
			wifiIrq = wifiIrqCheck;
		}
	} else {
		wifiIrqTimer = 0;
	}

	// calledViaIPC = false;
	// runCardEngineCheck();

	// Fix ARM9 VCount IRQ settings for color LUT and/or swap screens
	if ((valueBits & useColorLut) || (mainScreen > 0) || (screenIpc == 0x7)) {
		IPC_SendSync(screenIpc);
	}

	#ifndef TWLSDK
	fpsa_run();
	#endif

	if (sharedAddr[0] == 0x524F5245) { // 'EROR'
		REG_MASTER_VOLUME = 0;
		while (REG_VCOUNT != 191) swiDelay(100);
		while (REG_VCOUNT == 191) swiDelay(100);
	} else {
		break;
	}
  }
}

#ifndef TWLSDK
void i2cIRQHandler(void) {
	int cause = (i2cReadRegister(I2C_PM, I2CREGPM_PWRIF) & 0x3) | (i2cReadRegister(I2C_GPIO, 0x02)<<2);

	switch (cause & 3) {
	case 1: {
		if (saveTimer != 0) return;

		REG_MASTER_VOLUME = 0;
		int oldIME = enterCriticalSection();
		if (consoleModel < 2) {
			//unlaunchSetFilename(true);
			sharedAddr[4] = 0x57534352;
			IPC_SendSync(0x8);
			waitFrames(5);							// Wait for DSi screens to stabilize
		}
		rebootConsole();			// Reboot console
		leaveCriticalSection(oldIME);
		break;
	}
	case 2:
		writePowerManagement(PM_CONTROL_REG,PM_SYSTEM_PWR);
		break;
	}
}
#endif

u32 myIrqEnable(u32 irq) {	
	int oldIME = enterCriticalSection();

	#ifdef DEBUG		
	nocashMessage("myIrqEnable\n");
	#endif	

	initialize();

	//if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM)) {
		REG_AUXIE &= ~(1UL << 8);
	//}

	#ifdef TWLSDK
	//bool doBak = ((valueBits & gameOnFlashcard) && (valueBits & b_dsiSD));
	//if (doBak) bakSdData();
	#endif
	driveInitialize();
  	#ifdef TWLSDK
	//if (doBak) restoreSdBakData();
	#endif

	/*if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM) && ndsHeader->unitCode > 0 && (valueBits & dsiMode)) {
		extern u32* dsiIrqTable;
		extern u32* dsiIrqRet;
		extern u32* extraIrqTable_offset;
		extern u32* extraIrqRet_offset;

		dsiIrqTable[8] = extraIrqTable_offset[8];
		dsiIrqRet[8] = extraIrqRet_offset[8];
	}*/

	/*const char* romTid = getRomTid(ndsHeader);

	if ((strncmp(romTid, "UOR", 3) == 0)
	|| (strncmp(romTid, "UXB", 3) == 0)
	|| (strncmp(romTid, "USK", 3) == 0)
	|| (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM))) {
		// Proceed below "else" code
	} else {
		u32 irq_before = REG_IE;		
		REG_IE |= irq;
		leaveCriticalSection(oldIME);
		return irq_before;
	}*/

	u32 irq_before = REG_IE | IRQ_IPC_SYNC;
	irq |= IRQ_IPC_SYNC;
	//irq |= BIT(28);
	REG_IPC_SYNC |= IPC_SYNC_IRQ_ENABLE;

	REG_IE |= irq;
	//if (!(valueBits & powerCodeOnVBlank)) {
	//	REG_AUXIE |= IRQ_I2C;
	//}
	//if (!(valueBits & gameOnFlashcard) && !(valueBits & ROMinRAM)) {	
	//	REG_AUXIE |= IRQ_SDMMC;
	//}
	leaveCriticalSection(oldIME);
	//ipcSyncHooked = true;
	return irq_before;
}

/*static void irqIPCSYNCEnable(void) {	
	if (!initializedIRQ) {
		int oldIME = enterCriticalSection();
		initialize();	
		#ifdef DEBUG		
		dbg_printf("\nirqIPCSYNCEnable\n");	
		#endif	
		REG_IE |= IRQ_IPC_SYNC;
		REG_IPC_SYNC |= IPC_SYNC_IRQ_ENABLE;
		#ifdef DEBUG		
		dbg_printf("IRQ_IPC_SYNC enabled\n");
		#endif	
		leaveCriticalSection(oldIME);
		initializedIRQ = true;
	}
}*/

static inline void applyKeyRemap(u16* keyInput, u16* extKeyInput, const u8 remappedKey) {
	if (remappedKey >= 10) {
		*extKeyInput &= ~BIT(remappedKey);
	} else {
		*keyInput &= ~BIT(remappedKey);
	}
}

void patchKeyInputs(u16* extKeyInputDst, u16 extKeyInput) {
	u16 keyInput = *(u16*)0x04000130;
	const u16 keyInputBak = keyInput;
	const u16 extKeyInputBak = extKeyInput;
	keyInput = 0x3FF;
	for (int i = 10; i <= 11; i++) {
		extKeyInput |= BIT(i);
	}

	for (int i = 0; i <= 9; i++) {
		if (!(keyInputBak & BIT(i))) {
			applyKeyRemap(&keyInput, &extKeyInput, remappedKeys[i]);
		}
	}

	for (int i = 10; i <= 11; i++) {
		if (!(extKeyInputBak & BIT(i))) {
			applyKeyRemap(&keyInput, &extKeyInput, remappedKeys[i]);
		}
	}

	u32 dst = (u32)extKeyInputDst;
	dst -= 0x30;
	*(u16*)dst = keyInput;

	*extKeyInputDst = extKeyInput;
}

//
// ARM7 Redirected functions
//

bool eepromProtect(void) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromProtect\n");
	#endif

	return true;
}

bool eepromRead(u32 src, void *dst, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromRead\n");

	dbg_printf("\nsrc : \n");
	dbg_hexa(src);
	dbg_printf("\ndst : \n");
	dbg_hexa((u32)dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif

	if (!(valueBits & saveOnFlashcard) && isSdEjected()) {
		return false;
	}

	if (lockMutex(&saveMutex)) {
		// while (readOngoing) { swiDelay(100); }
		#ifdef TWLSDK
		//bool doBak = ((valueBits & gameOnFlashcard) && !(valueBits & saveOnFlashcard));
		//if (doBak) bakSdData();
		#endif
		//driveInitialize();
		/*if (saveInRam) {
			tonccpy(dst, (char*)0x02440000 + src, len);
		} else {*/
			sdmmc_set_ndma_slot(4);
			if ((u32)(src % saveSize)+len > saveSize) {
				u32 len2 = len;
				u32 len3 = 0;
				while ((u32)(src % saveSize)+len2 > saveSize) {
					len2--;
					len3++;
				}
				fileRead(dst, savFile, (src % saveSize), len2);
				fileRead(dst+len2, savFile, ((src+len2) % saveSize), len3);
			} else {
				fileRead(dst, savFile, (src % saveSize), len);
			}
			sdmmc_set_ndma_slot(0);
		//}
		#ifdef TWLSDK
		//if (doBak) restoreSdBakData();
		#endif
  		unlockMutex(&saveMutex);
	}
	return true;
}

bool eepromPageWrite(u32 dst, const void *src, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromPageWrite\n");

	dbg_printf("\nsrc : \n");
	dbg_hexa((u32)src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif

	if (!(valueBits & saveOnFlashcard) && isSdEjected()) {
		return false;
	}

	if (lockMutex(&saveMutex)) {
		// while (readOngoing) { swiDelay(100); }
		#ifdef TWLSDK
		//bool doBak = ((valueBits & gameOnFlashcard) && !(valueBits & saveOnFlashcard));
		//if (doBak) bakSdData();
		#endif
		//driveInitialize();
		saveTimer = 1;
		//i2cWriteRegister(0x4A, 0x12, 0x01);		// When we're saving, power button does nothing, in order to prevent corruption.
		/*if (saveInRam) {
			tonccpy((char*)0x02440000 + dst, src, len);
		}*/
		if (valueBits & delayWrites) {
			if (*(int*)((valueBits & isSdk5) ? 0x02FFFC3C : 0x027FFC3C) >= 60*2) {
				valueBits &= ~delayWrites;
			} else {
				waitFrames(1);
			}
		}
		sdmmc_set_ndma_slot(4);
		if ((dst % saveSize)+len > saveSize) {
			u32 len2 = len;
			u32 len3 = 0;
			while ((u32)(dst % saveSize)+len2 > saveSize) {
				len2--;
				len3++;
			}
			fileWrite(src, savFile, (dst % saveSize), len2);
			fileWrite(src+len2, savFile, ((dst+len2) % saveSize), len3);
		} else {
			fileWrite(src, savFile, (dst % saveSize), len);
		}
		sdmmc_set_ndma_slot(0);
		#ifdef TWLSDK
		//if (doBak) restoreSdBakData();
		#endif
  		unlockMutex(&saveMutex);
	}
	return true;
}

bool eepromPageProg(u32 dst, const void *src, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromPageProg\n");
	#endif

	return eepromPageWrite(dst, src, len);
}

bool eepromPageVerify(u32 dst, const void *src, u32 len) {
	#ifdef DEBUG
	dbg_printf("\narm7 eepromPageVerify\n");

	dbg_printf("\nsrc : \n");
	dbg_hexa((u32)src);
	dbg_printf("\ndst : \n");
	dbg_hexa(dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif

	return eepromPageWrite(dst, src, len);
}

bool eepromPageErase (u32 dst) {
	#ifdef DEBUG	
	dbg_printf("\narm7 eepromPageErase\n");	
	#endif	

	if (!(valueBits & saveOnFlashcard) && isSdEjected()) {
		return false;
	}

	// TODO: this should be implemented?
	return true;
}

/*
TODO: return the correct ID

From gbatek 
Returns RAW unencrypted Chip ID (eg. C2h,0Fh,00h,00h), repeated every 4 bytes.
  1st byte - Manufacturer (eg. C2h=Macronix) (roughly based on JEDEC IDs)
  2nd byte - Chip size (00h..7Fh: (N+1)Mbytes, F0h..FFh: (100h-N)*256Mbytes?)
  3rd byte - Flags (see below)
  4th byte - Flags (see below)
The Flag Bits in 3th byte can be
  0   Maybe Infrared flag? (in case ROM does contain on-chip infrared stuff)
  1   Unknown (set in some 3DS carts)
  2-7 Zero
The Flag Bits in 4th byte can be
  0-2 Zero
  3   Seems to be NAND flag (0=ROM, 1=NAND) (observed in only ONE cartridge)
  4   3DS Flag (0=NDS/DSi, 1=3DS)
  5   Zero   ... set in ... DSi-exclusive games?
  6   DSi flag (0=NDS/3DS, 1=DSi)
  7   Cart Protocol Variant (0=older/smaller carts, 1=newer/bigger carts)

Existing/known ROM IDs are:
  C2h,07h,00h,00h NDS Macronix 8MB ROM  (eg. DS Vision)
  AEh,0Fh,00h,00h NDS Noname   16MB ROM (eg. Meine Tierarztpraxis)
  C2h,0Fh,00h,00h NDS Macronix 16MB ROM (eg. Metroid Demo)
  C2h,1Fh,00h,00h NDS Macronix 32MB ROM (eg. Over the Hedge)
  C2h,1Fh,00h,40h DSi Macronix 32MB ROM (eg. Art Academy, TWL-VAAV, SystemFlaw)
  80h,3Fh,01h,E0h ?            64MB ROM+Infrared (eg. Walk with Me, NTR-IMWP)
  AEh,3Fh,00h,E0h DSi Noname   64MB ROM (eg. de Blob 2, TWL-VD2V)
  C2h,3Fh,00h,00h NDS Macronix 64MB ROM (eg. Ultimate Spiderman)
  C2h,3Fh,00h,40h DSi Macronix 64MB ROM (eg. Crime Lab, NTR-VAOP)
  80h,7Fh,00h,80h NDS SanDisk  128MB ROM (DS Zelda, NTR-AZEP-0)
  80h,7Fh,01h,E0h ?            128MB ROM+Infrared? (P-letter Soul Silver, IPGE)
  C2h,7Fh,00h,80h NDS Macronix 128MB ROM (eg. Spirit Tracks, NTR-BKIP)
  C2h,7Fh,00h,C0h DSi Macronix 128MB ROM (eg. Cooking Coach/TWL-VCKE)
  ECh,7Fh,00h,88h NDS Samsung  128MB NAND (eg. Warioware D.I.Y.)
  ECh,7Fh,01h,88h NDS Samsung? 128MB NAND+What? (eg. Jam with the Band, UXBP)
  ECh,7Fh,00h,E8h DSi Samsung? 128MB NAND (eg. Face Training, USKV)
  80h,FFh,80h,E0h NDS          256MB ROM (Kingdom Hearts - Re-Coded, NTR-BK9P)
  C2h,FFh,01h,C0h DSi Macronix 256MB ROM+Infrared? (eg. P-Letter White)
  C2h,FFh,00h,80h NDS Macronix 256MB ROM (eg. Band Hero, NTR-BGHP)
  C2h,FEh,01h,C0h DSi Macronix 512MB ROM+Infrared? (eg. P-Letter White 2)
  C2h,FEh,00h,90h 3DS Macronix probably 512MB? ROM (eg. Sims 3)
  45h,FAh,00h,90h 3DS SunDisk? maybe... 1.5GB? ROM (eg. Starfox)
  C2h,F8h,00h,90h 3DS Macronix maybe... 2GB?   ROM (eg. Kid Icarus)
  C2h,7Fh,00h,90h 3DS Macronix 128MB ROM CTR-P-AENJ MMinna no Ennichi
  C2h,FFh,00h,90h 3DS Macronix 256MB ROM CTR-P-AFSJ Pro Yakyuu Famista 2011
  C2h,FEh,00h,90h 3DS Macronix 512MB ROM CTR-P-AFAJ Real 3D Bass FishingFishOn
  C2h,FAh,00h,90h 3DS Macronix 1GB ROM CTR-P-ASUJ Hana to Ikimono Rittai Zukan
  C2h,FAh,02h,90h 3DS Macronix 1GB ROM CTR-P-AGGW Luigis Mansion 2 ASiA CHT
  C2h,F8h,00h,90h 3DS Macronix 2GB ROM CTR-P-ACFJ Castlevania - Lords of Shadow
  C2h,F8h,02h,90h 3DS Macronix 2GB ROM CTR-P-AH4J Monster Hunter 4
  AEh,FAh,00h,90h 3DS          1GB ROM CTR-P-AGKJ Gyakuten Saiban 5
  AEh,FAh,00h,98h 3DS          1GB NAND CTR-P-EGDJ Tobidase Doubutsu no Mori
  45h,FAh,00h,90h 3DS          1GB ROM CTR-P-AFLJ Fantasy Life
  45h,F8h,00h,90h 3DS          2GB ROM CTR-P-AVHJ Senran Kagura Burst - Guren
  C2h,F0h,00h,90h 3DS Macronix 4GB ROM CTR-P-ABRJ Biohazard Revelations
  FFh,FFh,FFh,FFh None (no cartridge inserted)
*/
u32 cardId(void) {
	#ifdef DEBUG	
	dbg_printf("\ncardId\n");
	#endif
    
    u32 cardid = getChipId(ndsHeader, moduleParams);

    //if (!cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0xE080FF80; // golden sun
    //if (!cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0x80FF80E0; // golden sun
    //if (cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0xFF000000; // golden sun
    //if (cardInitialized && strncmp(getRomTid(ndsHeader), "BO5", 3) == 0)  cardid = 0x000000FF; // golden sun

    #ifdef DEBUG
    dbg_hexa(cardid);
    #endif
    
	return cardid;
}

bool cardRead(u32 dma, u32 src, void *dst, u32 len) {
	#ifdef DEBUG	
	dbg_printf("\narm7 cardRead\n");	

	dbg_printf("\ndma : \n");
	dbg_hexa(dma);		
	dbg_printf("\nsrc : \n");
	dbg_hexa(src);		
	dbg_printf("\ndst : \n");
	dbg_hexa((u32)dst);
	dbg_printf("\nlen : \n");
	dbg_hexa(len);
	#endif	

	if (!cardReadRAM(dst, src, len)) {
		// while (readOngoing) { swiDelay(100); }
		//driveInitialize();
		cardReadLED(true, false);    // When a file is loading, turn on LED for card read indicator
		//ndmaUsed = false;
		#ifdef DEBUG	
		nocashMessage("fileRead romFile");
		#endif	
		fileRead(dst, romFile, src, len);
		//ndmaUsed = true;
		cardReadLED(false, false);    // After loading is done, turn off LED for card read indicator
	}

	return true;
}

#ifndef RA_ENGINE_H
#define RA_ENGINE_H

#ifndef RA_LINKER_SCRIPT
#include <nds/ndstypes.h>
#endif

// RetroAchievements on real hardware (DS games on DSi, SD mode).
//
// The loader stages everything in ramDump.bin, which a DSi RAM dump only
// fills to 16MB; the ARM7 card engine reads it into a 512KB block of the
// DSi's extra RAM that the ROM cache skips, and runs the rcheevos engine
// from there every VBlank.
//
// The block is addressed through the 0x0C000000 mirror, like the ROM cache:
// DS-mode games see 4MB repeated across 0x02000000-0x02FFFFFF, so
// 0x02E00000 would be the game's own RAM at 0x02200000.
//
// Region / staging layout (same offsets in both):
//   +0x00000  RaBootHeader
//   +0x00800  engine binary (cardenginei_arm7_ra.bin), linked here
//   +0x20000  achievement set text (see ra_connect.render_set)
//   +0x40000  engine heap (region only), stack at the top

#define RA_REGION             0x0CE00000
#define RA_ROM_CACHE_SKIP     RA_REGION
#define RA_REGION_SIZE        0x80000
#define RA_ENGINE_OFFSET      0x800
#define RA_ENGINE_MAX         (0x20000 - RA_ENGINE_OFFSET)
#define RA_SET_OFFSET         0x20000
#define RA_SET_MAX            0x20000
#define RA_HEAP_OFFSET        0x40000
#define RA_STACK_SIZE         0x4000

#define RA_DUMP_BOOT_OFFSET   0x01800000
#define RA_DUMP_UNLOCK_OFFSET 0x01FE0000
#define RA_DUMP_PROBE_OFFSET  0x01FF0000
#define RA_UNLOCK_RECORDS     1024

#define RA_BOOT_MAGIC   0x48424152 // 'RABH'
#define RA_ENGINE_MAGIC 0x4E454152 // 'RAEN'
#define RA_UNLOCK_MAGIC 0x31554152 // 'RAU1'

#define RA_MAX_DONE ((RA_ENGINE_OFFSET - 0x20) / 4)

#ifndef RA_LINKER_SCRIPT

struct RaBootHeader {
	u32 magic;
	u32 engineSize;
	u32 setSize;
	u32 unlockSeq;  // sequence number for the next unlock record
	u32 doneCount;  // achievements this game already unlocked: not loaded
	u32 reserved[3];
	u32 done[RA_MAX_DONE];
};

// One per unlock, in a ring in ramDump.bin; the loader moves them to
// sd:/_nds/ra/unlocks.log on the next boot.
struct RaUnlockRecord {
	u32 magic;
	u32 seq;
	u32 achievementId;
	u32 gameId;
	u32 points;
	u32 frame;      // VBlanks since the game started
	u8 rtc[8];      // year (from 2000), month, day, weekday, hour, minute, second, 0
	char md5[32];
};

struct RaStats {
	u32 achievements; // loaded from the set
	u32 parsed;
	u32 parseErrors;
	u32 unlocks;
	u32 frames;
	u32 lastLines;    // scanlines the last frame's evaluation took
	u32 maxLines;
	u32 heapUsed;
};

enum RaStatus {
	RA_LOCKED = 0,
	RA_UNLOCKED_BEFORE = 1, // in unlocks.log: not evaluated
	RA_UNLOCKED_NOW = 2,
	RA_UNSUPPORTED = 3,     // condition failed to parse
};

// The engine's table, also read by the achievements menu on ARM9.
struct RaAchievement {
	u32 id;
	u32 points;
	const char* title;
	const char* description;
	const char* memaddr;
	void* trigger;          // rc_trigger_t*, once parsed
	u32 status;             // enum RaStatus
};

// Card engine services the engine calls back into.
struct RaHost {
	void (*unlocked)(u32 achievementId, u32 points, const char* title);
	const struct RaBootHeader* boot;
	char* set;        // writable: parsed in place
	u32 setSize;
	// Called every few achievements while a frame is evaluated: the ARM7
	// serves the game's ROM reads, which must not wait for a whole frame.
	void (*poll)(void);
};

// First bytes of the engine binary.
struct RaEngineHeader {
	u32 magic;
	u32 version;
	int (*init)(const struct RaHost* host);
	void (*frame)(void);
	struct RaStats* stats;
	u32 gameId;
	char md5[33];
	char title[63];
	struct RaAchievement* achievements;
	u32 count;
};

#endif // RA_LINKER_SCRIPT

#endif // RA_ENGINE_H

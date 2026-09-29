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
//   +0x80000  in-game network stack (ranet.bin from ra-nds, ra_netblob.h),
//             its memory, and at the very end its staged settings

#define RA_REGION             0x0CE00000
#define RA_ROM_CACHE_SKIP     RA_REGION
#define RA_REGION_SIZE        0x100000
#define RA_ENGINE_AREA        0x80000 // header, engine, set, heap and stack
#define RA_NET_OFFSET         0x80000 // = RA_NET_BLOB_ADDRESS - RA_REGION
#define RA_ENGINE_OFFSET      0x800
#define RA_ENGINE_MAX         (0x20000 - RA_ENGINE_OFFSET)
// Console key for signing unlocks: SHA-256 of the build secret
// (ra_secret.h), the eMMC CID (RaBootHeader.cid) and RA_KEY_LABEL.  The
// engine derives it at start (RAM written by the loader doesn't survive
// the boot), the loader and RA Sync (ra-nds) derive the same; it is never
// written to the SD card.
#define RA_KEY_LABEL          "RetroAchievements DSi console key 1"
#define RA_SET_OFFSET         0x20000
#define RA_SET_MAX            0x20000
#define RA_HEAP_OFFSET        0x40000
#define RA_STACK_SIZE         0x4000

// Fast-RAM variant (config "wram"): the engine's code, constants, working
// data and stack in the ARM7's DSi WRAM-A below the cheat engine, so its
// instruction fetches stay off the main memory bus the game's ARM9 needs.
// Free unless the game's WiFi binary was relocated there
// (hasVramWifiBinary).  Header, .data, achievements table and stats stay
// in RA_REGION, where the ARM9 achievements list reads them.
#define RA_WRAM_CODE          0x037C0000
#define RA_WRAM_SIZE          0x1C000 // up to CHEAT_ENGINE_LOCATION

#define RA_DUMP_BOOT_OFFSET   0x01800000
// Staging only, after the set: the fast-RAM variant's two parts
#define RA_STAGE_WRAM_MAIN    0x40000
#define RA_STAGE_WRAM_CODE    0x60000
// ...and the in-game network stack: ranet.bin, then struct RaNetStage
#define RA_STAGE_NET_BLOB     0x80000
#define RA_NET_BLOB_MAX       0x60000
#define RA_STAGE_NET_DATA     0xE0000
// Where the card engine keeps RaNetStage until the blob copies it: the
// end of the blob's area, which its image and .bss must stay below
#define RA_NET_STAGE_TEMP     (RA_REGION + RA_REGION_SIZE - 0x2000)
// ...and its log below that (RA_NETLOG_SIZE): the blob must end before it
#define RA_NET_LOG_RAM        (RA_NET_STAGE_TEMP - RA_NETLOG_SIZE)
// Softcore/hardcore chosen in the in-game menu, for the next start:
// { RA_MODE_MAGIC, 0 or 1 }.  Overrides config.txt "hardcore".
#define RA_DUMP_MODE_OFFSET   0x01FD0000
#define RA_MODE_MAGIC         0x4F4D4152 // 'RAMO'
// Real-time upload switched in the in-game menu, for the loader to
// remember per game (sd:/_nds/ra/realtime_off.txt, by hash): struct
// RaRealtimeChoice
#define RA_DUMP_REALTIME_OFFSET (RA_DUMP_MODE_OFFSET + 0x10)
#define RA_REALTIME_MAGIC     0x54524152 // 'RART'
#define RA_DUMP_UNLOCK_OFFSET 0x01FE0000
#define RA_DUMP_PROBE_OFFSET  0x01FF0000
#define RA_UNLOCK_RECORDS     512 // struct RaSignedUnlock, 96 bytes each
// After the unlock ring (48K): what the in-game network stack did with each
// unlock, { seq, enum RaNetAward } per ring slot, for RA Sync; then its log
#define RA_DUMP_SENT_OFFSET   0x01FEC000
#define RA_DUMP_NETLOG_OFFSET 0x01FED000
#define RA_NETLOG_SIZE        0x3000

#define RA_BOOT_MAGIC   0x48424152 // 'RABH'
#define RA_ENGINE_MAGIC 0x4E454152 // 'RAEN'
#define RA_UNLOCK_MAGIC 0x32554152 // 'RAU2': signed (struct RaSignedUnlock)

#define RA_MAX_DONE ((RA_ENGINE_OFFSET - 0x30) / 4)


// RaBootHeader.config, from sd:/_nds/ra/config.txt
#define RA_CFG_WRAM           (1 << 0)   // use the fast-RAM variant if it fits
#define RA_CFG_PRIO_SHIFT     1          // ARM9 main-memory priority:
#define RA_CFG_PRIO_MASK      (3 << 1)
#define RA_PRIO_DURING        0          //   only while a frame is evaluated
#define RA_PRIO_ALWAYS        1          //   from 5 s in, re-applied every second
#define RA_PRIO_OFF           2          //   never touched
// Hardcore is built but off until RetroAchievements accepts this client:
// the loader ignores it in config.txt and the menu, and the menu hides it.
#define RA_HARDCORE_AVAILABLE 0
#define RA_CFG_HARDCORE       (1 << 3)   // no cheats, RAM viewer/editor or refresh-rate change
#define RA_CFG_NET            (1 << 4)   // in-game sending staged (RaNetStage)
#define RA_CFG_NET_OFF        (1 << 5)   // ...but switched off (in-game menu)
#define RA_CFG_INTERVAL_SHIFT 8          // evaluate every Nth frame (0/1: all)
#define RA_CFG_DEFAULT        (RA_CFG_WRAM | RA_CFG_NET)

#ifndef RA_LINKER_SCRIPT

struct RaBootHeader {
	u32 magic;
	u32 engineSize;
	u32 setSize;
	u32 unlockSeq;  // sequence number for the next unlock record
	u32 doneCount;  // achievements this game already unlocked: not loaded
	u32 config;       // RA_CFG_*
	u32 wramMainSize; // fast-RAM variant parts, staged at RA_STAGE_WRAM_*
	u32 wramCodeSize; //   (0: not staged)
	u8 cid[16];       // eMMC CID, for the console key (all zero: none)
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
	u8 rtc[8];      // year (from 2000), month, day, weekday, hour, minute, second,
	                // then flags: bit 0 = earned in hardcore
	char md5[32];
};

// An unlock record and its HMAC-SHA256 under the console key, as the engine
// writes it to the ring and sd:/_nds/ra/unlocks.bin keeps it.  Records that
// don't verify are never sent.
struct RaSignedUnlock {
	struct RaUnlockRecord record;
	u8 mac[32];
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
	// HMAC-SHA256 of data under the console key (zeros without a CID,
	// which no check accepts)
	void (*sign)(const void* data, u32 size, u8 mac[32]);
};

// In-game sending (RA_CFG_NET): what the loader stages for ranet.bin
// (ra-nds; ra_netblob.h) at RA_STAGE_NET_DATA.  Holds the RA token and the
// TLS session secret, like account.txt and tls.bin already on the card.
#define RA_NET_STAGE_MAGIC 0x534E4152 // 'RANS'
#include "ra_netblob.h"
#include "ra_netprofile.h"
#include "ra_tlssession.h"
struct RaNetStage {
	u32 magic;
	u32 blobSize;       // bytes staged at RA_STAGE_NET_BLOB
	char user[RA_NET_USER_MAX];
	char token[RA_NET_TOKEN_MAX];
	RaNetProfile profile;
	RaTlsSession tls;
};

struct RaRealtimeChoice {
	u32 magic;    // RA_REALTIME_MAGIC
	u32 on;
	char md5[32]; // the game's hash
};

// What the in-game network stack did with an unlock (RA_DUMP_SENT_OFFSET,
// slot seq % RA_UNLOCK_RECORDS)
struct RaNetSent {
	u32 seq;
	u32 result; // enum RaNetAward
};

#endif // RA_LINKER_SCRIPT

#endif // RA_ENGINE_H

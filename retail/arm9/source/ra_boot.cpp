// RetroAchievements: stage the engine and this game's set for the ARM7 card
// engine, and move last session's unlocks to sd:/_nds/ra/unlocks.log.
// See ra_engine.h for the layout.

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#include <sys/stat.h>
#include "myDSiMode.h"
#include "configuration.h"
#include "ra_engine.h"

#define RA_DIR "sd:/_nds/ra"
#define RA_LOG RA_DIR "/unlocks.log"

// Append last session's unlock records to unlocks.log, then clear the ring.
// Returns the number of lines in the log afterwards.
static u32 flushUnlocks(FILE* dump) {
	std::vector<RaUnlockRecord> records(RA_UNLOCK_RECORDS);
	fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET);
	const size_t got = fread(records.data(), sizeof(RaUnlockRecord), RA_UNLOCK_RECORDS, dump);

	std::vector<RaUnlockRecord> valid;
	for (size_t i = 0; i < got; i++) {
		if (records[i].magic == RA_UNLOCK_MAGIC) {
			valid.push_back(records[i]);
		}
	}
	std::sort(valid.begin(), valid.end(),
		[](const RaUnlockRecord& a, const RaUnlockRecord& b) { return a.seq < b.seq; });

	if (!valid.empty()) {
		mkdir(RA_DIR, 0777);
		FILE* log = fopen(RA_LOG, "ab");
		if (!log) {
			return 0; // keep the ring for next time
		}
		for (const RaUnlockRecord& r : valid) {
			char md5[33];
			memcpy(md5, r.md5, 32);
			md5[32] = 0;
			// achievement, game, md5, points, local time, frame
			fprintf(log, "%lu\t%lu\t%s\t%lu\t20%02lu-%02lu-%02lu %02lu:%02lu:%02lu\t%lu\n",
				r.achievementId, r.gameId, md5, r.points,
				(u32)r.rtc[0], (u32)r.rtc[1], (u32)r.rtc[2],
				(u32)r.rtc[4], (u32)r.rtc[5], (u32)r.rtc[6], r.frame);
		}
		fclose(log);

		std::vector<u8> zero(RA_UNLOCK_RECORDS * sizeof(RaUnlockRecord), 0);
		fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET);
		fwrite(zero.data(), 1, zero.size(), dump);
	}

	u32 lines = 0;
	FILE* log = fopen(RA_LOG, "rb");
	if (log) {
		int c;
		while ((c = fgetc(log)) != EOF) {
			if (c == '\n') lines++;
		}
		fclose(log);
	}
	return lines;
}

// Achievement ids already unlocked for this ROM, from unlocks.log
static void collectDone(const char* md5, RaBootHeader* boot) {
	FILE* log = fopen(RA_LOG, "rb");
	if (!log) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), log) && boot->doneCount < RA_MAX_DONE) {
		char* tab1 = strchr(line, '\t');
		char* tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
		if (tab2 && strncmp(tab2 + 1, md5, 32) == 0) {
			boot->done[boot->doneCount++] = strtoul(line, NULL, 10);
		}
	}
	fclose(log);
}

static bool readFile(const char* path, std::vector<u8>& out, size_t max) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size_t)size > max) {
		fclose(f);
		return false;
	}
	out.resize(size);
	const bool ok = fread(out.data(), 1, size, f) == (size_t)size;
	fclose(f);
	return ok;
}

// sd:/_nds/ra/config.txt: "key=value" lines, for trying engine variants
//   wram=1|0                    fast-RAM engine when the game leaves room (1)
//   priority=during|always|off  ARM9 main-memory priority (during)
//   interval=N                  evaluate every Nth frame (1)
static u32 readConfig(void) {
	u32 config = RA_CFG_DEFAULT;
	FILE* f = fopen(RA_DIR "/config.txt", "rb");
	if (!f) {
		return config;
	}
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#') {
			continue;
		}
		*eq = 0;
		char* value = eq + 1;
		value[strcspn(value, "\r\n \t#")] = 0;
		if (strcmp(line, "wram") == 0) {
			config = (strcmp(value, "0") == 0) ? (config & ~RA_CFG_WRAM) : (config | RA_CFG_WRAM);
		} else if (strcmp(line, "priority") == 0) {
			const u32 prio = strcmp(value, "always") == 0 ? RA_PRIO_ALWAYS
			               : strcmp(value, "off") == 0 ? RA_PRIO_OFF : RA_PRIO_DURING;
			config = (config & ~RA_CFG_PRIO_MASK) | (prio << RA_CFG_PRIO_SHIFT);
		} else if (strcmp(line, "interval") == 0) {
			u32 n = strtoul(value, NULL, 10);
			if (n > 255) n = 255;
			config = (config & ~(0xFFu << RA_CFG_INTERVAL_SHIFT)) | (n << RA_CFG_INTERVAL_SHIFT);
		}
	}
	fclose(f);
	return config;
}

void raPrepareBoot(const configuration* conf, const std::string& ramDumpPath) {
	if (!dsiFeatures() || conf->b4dsMode || conf->bootstrapOnFlashcard || conf->gameOnFlashcard) {
		return;
	}
	FILE* dump = fopen(ramDumpPath.c_str(), "r+b");
	if (!dump) {
		return;
	}

	RaBootHeader* boot = (RaBootHeader*)calloc(1, RA_ENGINE_OFFSET);
	boot->unlockSeq = flushUnlocks(dump);

	// sd:/_nds/ra/sets/<ROM file name>.txt, written by GameSync
	const char* name = strrchr(conf->ndsPath, '/');
	name = name ? name + 1 : conf->ndsPath;
	std::string setPath = std::string(RA_DIR "/sets/") + name + ".txt";

	std::vector<u8> set, engine;
	if (readFile(setPath.c_str(), set, RA_SET_MAX - 1)
	 && readFile("nitro:/cardenginei_arm7_ra.bin", engine, RA_ENGINE_MAX)) {
		// "game\t<id>\t<md5>\t<title>" is the second line
		set.push_back(0); // for strstr; not written out
		const char* game = strstr((const char*)set.data(), "\ngame\t");
		const char* md5 = game ? strchr(game + 6, '\t') : NULL;
		if (md5) {
			collectDone(md5 + 1, boot);
		}
		set.pop_back();
		boot->magic = RA_BOOT_MAGIC;
		boot->engineSize = engine.size();
		boot->setSize = set.size();
		boot->config = readConfig();

		// Fast-RAM variant: the card engine picks it if the game leaves room
		std::vector<u8> wramMain, wramCode;
		if (readFile("nitro:/cardenginei_arm7_ra_wram_main.bin", wramMain, RA_ENGINE_MAX)
		 && readFile("nitro:/cardenginei_arm7_ra_wram_code.bin", wramCode, RA_WRAM_SIZE - RA_STACK_SIZE)) {
			boot->wramMainSize = wramMain.size();
			boot->wramCodeSize = wramCode.size();
			fseek(dump, RA_DUMP_BOOT_OFFSET + RA_STAGE_WRAM_MAIN, SEEK_SET);
			fwrite(wramMain.data(), 1, wramMain.size(), dump);
			fseek(dump, RA_DUMP_BOOT_OFFSET + RA_STAGE_WRAM_CODE, SEEK_SET);
			fwrite(wramCode.data(), 1, wramCode.size(), dump);
		}

		fseek(dump, RA_DUMP_BOOT_OFFSET + RA_ENGINE_OFFSET, SEEK_SET);
		fwrite(engine.data(), 1, engine.size(), dump);
		fseek(dump, RA_DUMP_BOOT_OFFSET + RA_SET_OFFSET, SEEK_SET);
		fwrite(set.data(), 1, set.size(), dump);
	}

	// Always written: a stale header from another game must not load
	fseek(dump, RA_DUMP_BOOT_OFFSET, SEEK_SET);
	fwrite(boot, 1, RA_ENGINE_OFFSET, dump);
	fclose(dump);
	free(boot);
}

#define RA_SYNC_PATH "sd:/_nds/ra/rasync.nds"
#define RA_RETURN_PATH RA_DIR "/return.txt"
#define RA_PREP_PATH RA_DIR "/raprep.nds"
#define RA_PREP_FILE RA_DIR "/prep.txt"
#define RA_SKIP_FILE RA_DIR "/skip_once.txt"

// Unlaunch's auto-load request: once the console restarts, Unlaunch boots
// `path` instead of its default.  (This loader runs at 0x02280000, so the
// block at 0x02000800 is free.)
static void unlaunchAutoload(const char* path) {
	u8* info = (u8*)0x02000800;
	memcpy(info, "AutoLoadInfo", 12);
	*(u16*)(info + 0x0C) = 0x3F0;			// length covered by the CRC
	*(u16*)(info + 0x0E) = 0;				// CRC, below
	*(u32*)(info + 0x10) = BIT(0) | BIT(1);	// load the path; use the colours
	*(u16*)(info + 0x14) = 0x7FFF;			// top screen colour
	*(u16*)(info + 0x16) = 0x7FFF;			// bottom screen colour
	memset(info + 0x18, 0, 0x20 + 0x208 + 0x1C0);
	u16* name = (u16*)(info + 0x38);		// UTF-16, 0-terminated
	for (int i = 0; i < 255 && path[i]; i++) {
		name[i] = (u8)path[i];
	}
	*(u16*)(info + 0x0E) = swiCRC16(0xFFFF, info + 0x10, 0x3F0);
	DC_FlushAll();
}

// A game with no achievement set yet goes through RA Prep first: the loader
// saves the game's path in prep.txt and restarts the console into
// raprep.nds (through Unlaunch, in full DSi mode for WPA2), which looks the
// ROM up on RetroAchievements, writes its set (or <rom>.none when RA has no
// set for it) and starts this loader again.  If the fetch failed, raprep
// writes skip_once.txt so that the next start plays without a set instead of
// trying again at once.  Only returns when the game should just start.
void raRedirectPrep(configuration* conf, const char* bootstrapPath) {
	if (!conf->ndsPath || strncmp(conf->ndsPath, "sd:/", 4) != 0 || !isDSiMode()) {
		return;
	}
	struct stat st;
	if (stat(RA_PREP_PATH, &st) != 0) {
		return;
	}

	// One start without a set after a failed fetch
	FILE* f = fopen(RA_SKIP_FILE, "rb");
	if (f) {
		char skip[256] = {0};
		fgets(skip, sizeof(skip), f);
		fclose(f);
		remove(RA_SKIP_FILE);
		skip[strcspn(skip, "\r\n")] = '\0';
		if (strcmp(skip, conf->ndsPath) == 0) {
			return;
		}
	}

	const char* name = strrchr(conf->ndsPath, '/');
	name = name ? name + 1 : conf->ndsPath;
	const std::string setPath = std::string(RA_DIR "/sets/") + name;
	if (stat((setPath + ".txt").c_str(), &st) == 0 || stat((setPath + ".none").c_str(), &st) == 0) {
		return;
	}

	f = fopen(RA_PREP_FILE, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "%s\n%s\n", conf->ndsPath,
		strncmp(bootstrapPath, "sd:/", 4) == 0 ? bootstrapPath : "sd:/_nds/nds-bootstrap-nightly.nds");
	fclose(f);

	unlaunchAutoload(RA_PREP_PATH);
	// The ARM7 restarts the console (retail/arm7/source/main.c); the FIFO
	// message only wakes it up
	*(vu32*)0x0CFFFD0C = 0x544F4252; // 'RBOT'
	fifoSendValue32(FIFO_USER_08, 0);
	while (1) {
		swiDelay(100);
	}
}

// Quitting a game with achievements goes through RA Sync (GameSync's
// rasync.nds): it uploads the unlocks over the DSi's WiFi, fetches sets for
// new ROMs, then boots the original quit target, saved in return.txt.
// Quitting boots the quit path through Unlaunch in full DSi mode, which the
// DSi WiFi (WPA2) needs.
void raRedirectQuit(configuration* conf) {
	if (!conf->ndsPath || strncmp(conf->ndsPath, "sd:/", 4) != 0
	 || !conf->quitPath || !conf->quitPath[0] || strcmp(conf->quitPath, RA_SYNC_PATH) == 0) {
		return;
	}
	const char* name = strrchr(conf->ndsPath, '/');
	name = name ? name + 1 : conf->ndsPath;
	const std::string setPath = std::string(RA_DIR "/sets/") + name + ".txt";
	struct stat st;
	if (stat(setPath.c_str(), &st) != 0 || stat(RA_SYNC_PATH, &st) != 0) {
		return;
	}
	FILE* f = fopen(RA_RETURN_PATH, "wb");
	if (!f) {
		return;
	}
	fputs(conf->quitPath, f);
	fclose(f);
	conf->quitPath = strdup(RA_SYNC_PATH);
}

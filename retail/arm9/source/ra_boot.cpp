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
		const char* game = strstr((const char*)set.data(), "\ngame\t");
		const char* md5 = game ? strchr(game + 6, '\t') : NULL;
		if (md5) {
			collectDone(md5 + 1, boot);
		}
		boot->magic = RA_BOOT_MAGIC;
		boot->engineSize = engine.size();
		boot->setSize = set.size();

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

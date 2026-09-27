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

static u32 le32(const u8* p) {
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

// sd:/_nds/ra/unlock.wav, played with the unlock popup: staged as mono
// signed PCM for the DS sound hardware, 16-bit if it fits RA_SOUND_MAX, else
// 8-bit; rates above 32 kHz are divided down, and a long sound is cut.
// Without it the card engine plays a short tone arpeggio.
static void stageUnlockSound(FILE* dump, RaBootHeader* boot) {
	std::vector<u8> wav;
	if (!readFile(RA_DIR "/unlock.wav", wav, 1024 * 1024) || wav.size() < 44
	 || memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0) {
		return;
	}
	u32 format = 0, channels = 0, rate = 0, bits = 0, dataSize = 0;
	const u8* data = NULL;
	for (size_t pos = 12; pos + 8 <= wav.size();) {
		const u8* chunk = wav.data() + pos;
		u32 size = le32(chunk + 4);
		if (size > wav.size() - pos - 8) {
			size = wav.size() - pos - 8;
		}
		if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
			format = chunk[8] | (chunk[9] << 8);
			channels = chunk[10] | (chunk[11] << 8);
			rate = le32(chunk + 12);
			bits = chunk[22] | (chunk[23] << 8);
		} else if (memcmp(chunk, "data", 4) == 0) {
			data = chunk + 8;
			dataSize = size;
		}
		pos += 8 + size + (size & 1);
	}
	if (format != 1 || !data || (bits != 8 && bits != 16) || channels < 1 || channels > 2
	 || rate < 4000 || rate > 96000) {
		return;
	}
	const u32 frameBytes = channels * bits / 8;
	u32 step = 1;
	while (rate / step > 32000) {
		step++;
	}
	u32 frames = dataSize / frameBytes / step;
	const bool pcm16 = frames * 2 <= RA_SOUND_MAX;
	const u32 maxFrames = pcm16 ? RA_SOUND_MAX / 2 : RA_SOUND_MAX;
	if (frames > maxFrames) {
		frames = maxFrames;
	}
	std::vector<u8> out(((pcm16 ? frames * 2 : frames) + 3) & ~3); // whole words
	for (u32 i = 0; i < frames; i++) {
		s32 sum = 0;
		for (u32 s = 0; s < step; s++) {
			const u8* f = data + (i * step + s) * frameBytes;
			for (u32 c = 0; c < channels; c++) {
				sum += (bits == 16) ? (s16)(f[c * 2] | (f[c * 2 + 1] << 8)) : ((s32)f[c] - 128) * 256;
			}
		}
		const s32 v = sum / (s32)(step * channels);
		if (pcm16) {
			out[i * 2] = v & 0xFF;
			out[i * 2 + 1] = (v >> 8) & 0xFF;
		} else {
			out[i] = (u8)(s8)(v >> 8);
		}
	}
	fseek(dump, RA_DUMP_BOOT_OFFSET + RA_STAGE_SOUND, SEEK_SET);
	if (fwrite(out.data(), 1, out.size(), dump) == out.size()) {
		boot->soundSize = out.size();
		boot->soundFormat = (rate / step) | (pcm16 ? RA_SOUND_PCM16 : 0);
	}
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

		stageUnlockSound(dump, boot);

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
#define RA_AFTER_PREP_FILE RA_DIR "/after_prep.txt"

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

// The ROM file's fingerprint: size, header CRC and modification time (hex),
// all free to read here.  Another dump (region, revision) changes the size
// or header; a replaced or patched file changes the time.
static bool romFingerprint(const char* romPath, char* out, size_t size) {
	struct stat st;
	u16 crc = 0;
	FILE* f = fopen(romPath, "rb");
	if (!f || stat(romPath, &st) != 0) {
		if (f) fclose(f);
		return false;
	}
	fseek(f, 0x15E, SEEK_SET);
	fread(&crc, sizeof(crc), 1, f);
	fclose(f);
	snprintf(out, size, "%lx %04x %lx", (unsigned long)st.st_size, crc, (unsigned long)st.st_mtime);
	return true;
}

// Whether the ROM is still the one its set was made for: RA Prep keeps the
// fingerprint this loader gave it on the first line of <rom>.id (then the
// ROM's hash).  Missing .id: not checked yet.
static bool romMatchesId(const char* fingerprint, const char* idPath) {
	char expected[64] = {0};
	FILE* f = fopen(idPath, "rb");
	if (!f) {
		return false;
	}
	fgets(expected, sizeof(expected), f);
	fclose(f);
	expected[strcspn(expected, "\r\n")] = '\0';
	return strcmp(fingerprint, expected) == 0;
}

// A game with no achievement set yet goes through RA Prep first: the loader
// saves the game's path in prep.txt and restarts the console into
// raprep.nds (through Unlaunch, in full DSi mode for WPA2), which looks the
// ROM up on RetroAchievements, writes its set (or <rom>.none when RA has no
// set for it) and starts this loader again.  If the fetch failed, raprep
// writes skip_once.txt so that the next start plays without a set instead of
// trying again at once.  A set whose <rom>.id doesn't match the ROM (or has
// none yet) goes through RA Prep too, which hashes the ROM again and keeps
// or replaces the set.  Only returns when the game should just start.
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
	char fingerprint[64];
	if (!romFingerprint(conf->ndsPath, fingerprint, sizeof(fingerprint))) {
		return;
	}
	if ((stat((setPath + ".txt").c_str(), &st) == 0 || stat((setPath + ".none").c_str(), &st) == 0)
	 && romMatchesId(fingerprint, (setPath + ".id").c_str())) {
		return;
	}

	// ROM, loader to start again, fingerprint for <rom>.id
	f = fopen(RA_PREP_FILE, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "%s\n%s\n%s\n", conf->ndsPath,
		strncmp(bootstrapPath, "sd:/", 4) == 0 ? bootstrapPath : "sd:/_nds/nds-bootstrap-nightly.nds",
		fingerprint);
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
	// Started by RA Prep's restart (after_prep.txt names this ROM): quitting
	// straight to TWiLight Menu++ then hangs on the game's last frame, while
	// RA Sync, which returns through Unlaunch, works, so quit through RA Sync
	// even without a set.  One use.
	bool afterPrep = false;
	FILE* f = fopen(RA_AFTER_PREP_FILE, "rb");
	if (f) {
		char rom[256] = {0};
		fgets(rom, sizeof(rom), f);
		fclose(f);
		remove(RA_AFTER_PREP_FILE);
		rom[strcspn(rom, "\r\n")] = '\0';
		afterPrep = strcmp(rom, conf->ndsPath) == 0;
	}

	const char* name = strrchr(conf->ndsPath, '/');
	name = name ? name + 1 : conf->ndsPath;
	const std::string setPath = std::string(RA_DIR "/sets/") + name + ".txt";
	struct stat st;
	if ((!afterPrep && stat(setPath.c_str(), &st) != 0) || stat(RA_SYNC_PATH, &st) != 0) {
		return;
	}
	f = fopen(RA_RETURN_PATH, "wb");
	if (!f) {
		return;
	}
	fputs(conf->quitPath, f);
	fclose(f);
	conf->quitPath = strdup(RA_SYNC_PATH);
}

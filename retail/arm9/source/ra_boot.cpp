// RetroAchievements: stage the engine and this game's set for the ARM7 card
// engine, and move last session's signed unlocks to sd:/_nds/ra/unlocks.bin.
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
#include "ra_sha256.h"
#include "ra_secret.h"

#define RA_DIR "sd:/_nds/ra"
#define RA_SETS RA_DIR "/sets/"
#define RA_UNLOCKS RA_DIR "/unlocks.bin"
#define RA_HISTORY RA_DIR "/unlocks_history.txt"

// ---------------------------------------------------------------------------
// Console key: SHA-256 of the build secret (ra_secret.h, not in git) and
// the eMMC CID, which the DSi keeps at 0x02FFD7BC for its programs.  RA Sync
// (ra-nds) derives the same key.  It signs the unlock records and the
// sets; it stays in RAM and never goes to the SD card.
// ---------------------------------------------------------------------------

static const char raKeyLabel[] = RA_KEY_LABEL;
static u8 raKey[32];
static u8 raCid[16]; // for the engine, which derives the key itself
static int raKeyState = 0; // 0: not tried, 1: ready, -1: no CID

static bool deriveKey(void) {
	if (raKeyState) {
		return raKeyState > 0;
	}
	vu8* cid = (vu8*)0x02FFD7BC;
	bool present = false;
	for (int i = 0; i < 16; i++) present |= cid[i] != 0;
	if (!present) {
		// Not filled in yet: the loader ARM7 reads it (retail/arm7/source/main.c)
		*(vu32*)0x0CFFFD0C = 0x454D4D43; // 'EMMC'
		fifoSendValue32(FIFO_USER_08, 0);
		for (int i = 0; i < 1000 && *(vu32*)0x0CFFFD0C != 0; i++) {
			swiDelay(1000);
		}
		DC_InvalidateRange((void*)0x02FFD7BC, 32);
		for (int i = 0; i < 16; i++) present |= cid[i] != 0;
	}
	if (!present) {
		raKeyState = -1;
		return false;
	}
	u8* id = raCid;
	for (int i = 0; i < 16; i++) id[i] = cid[i];
	RaSha256 s;
	raSha256Init(&s);
	raSha256Update(&s, raBuildSecret, sizeof(raBuildSecret));
	raSha256Update(&s, id, sizeof(raCid));
	raSha256Update(&s, raKeyLabel, sizeof(raKeyLabel) - 1);
	raSha256Final(&s, raKey);
	raKeyState = 1;
	return true;
}

static bool verifyUnlock(const RaSignedUnlock& u) {
	if (u.record.magic != RA_UNLOCK_MAGIC || !deriveKey()) {
		return false;
	}
	u8 mac[32];
	raHmacSha256(raKey, &u.record, sizeof(u.record), mac);
	return memcmp(mac, u.mac, 32) == 0;
}

// <rom>.sig: the set's HMAC under the console key, in hex, written by RA
// Prep.  A set edited on the SD card (or from another console, or fetched
// before signing existed) doesn't match and isn't loaded.
static bool setSignatureOk(const char* name, const std::vector<u8>& set) {
	if (!deriveKey()) {
		return false;
	}
	char stored[80] = {0};
	FILE* f = fopen((std::string(RA_SETS) + name + ".sig").c_str(), "rb");
	if (!f) {
		return false;
	}
	fgets(stored, sizeof(stored), f);
	fclose(f);
	u8 mac[32];
	raHmacSha256(raKey, set.data(), set.size(), mac);
	char hex[65];
	for (int i = 0; i < 32; i++) {
		snprintf(hex + i * 2, 3, "%02x", mac[i]);
	}
	return strncmp(stored, hex, 64) == 0;
}

// Last session's unlock records, from the ring in ramDump.bin to
// unlocks.bin (only those that verify), with a line each in
// unlocks_history.txt for reading.  Then the ring is cleared.  Returns the
// number of records in unlocks.bin: the next sequence number.
static u32 flushUnlocks(FILE* dump) {
	struct stat st;
	if (!deriveKey()) {
		// Can't check them: leave the ring alone
		return stat(RA_UNLOCKS, &st) == 0 ? st.st_size / sizeof(RaSignedUnlock) : 0;
	}
	std::vector<RaSignedUnlock> ring(RA_UNLOCK_RECORDS);
	fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET);
	const size_t got = fread(ring.data(), sizeof(RaSignedUnlock), RA_UNLOCK_RECORDS, dump);

	std::vector<RaSignedUnlock> valid;
	u32 rejected = 0;
	for (size_t i = 0; i < got; i++) {
		if (ring[i].record.magic != RA_UNLOCK_MAGIC) {
			continue;
		}
		if (verifyUnlock(ring[i])) {
			valid.push_back(ring[i]);
		} else {
			rejected++;
		}
	}
	std::sort(valid.begin(), valid.end(),
		[](const RaSignedUnlock& a, const RaSignedUnlock& b) { return a.record.seq < b.record.seq; });

	if (!valid.empty() || rejected) {
		mkdir(RA_DIR, 0777);
		FILE* bin = fopen(RA_UNLOCKS, "ab");
		if (!bin) {
			return 0; // keep the ring for next time
		}
		fwrite(valid.data(), sizeof(RaSignedUnlock), valid.size(), bin);
		fclose(bin);
		FILE* log = fopen(RA_HISTORY, "ab");
		if (log) {
			for (const RaSignedUnlock& u : valid) {
				const RaUnlockRecord& r = u.record;
				fprintf(log, "20%02u-%02u-%02u %02u:%02u:%02u\t%lu\t%lu\t%.32s\t%lu\t%s\n",
					r.rtc[0], r.rtc[1], r.rtc[2], r.rtc[4], r.rtc[5], r.rtc[6],
					r.achievementId, r.gameId, r.md5, r.points, (r.rtc[7] & 1) ? "hardcore" : "softcore");
			}
			if (rejected) {
				fprintf(log, "(%lu records failed their signature check and were dropped)\n", rejected);
			}
			fclose(log);
		}
		std::vector<u8> zero(RA_UNLOCK_RECORDS * sizeof(RaSignedUnlock), 0);
		fseek(dump, RA_DUMP_UNLOCK_OFFSET, SEEK_SET);
		fwrite(zero.data(), 1, zero.size(), dump);
	}

	return stat(RA_UNLOCKS, &st) == 0 ? st.st_size / sizeof(RaSignedUnlock) : 0;
}

static void addDone(RaBootHeader* boot, u32 id) {
	for (u32 i = 0; i < boot->doneCount; i++) {
		if (boot->done[i] == id) return;
	}
	if (boot->doneCount < RA_MAX_DONE) {
		boot->done[boot->doneCount++] = id;
	}
}

// Achievements already unlocked for this ROM in the mode being played: in
// hardcore only hardcore unlocks count, in softcore both (a hardcore unlock
// is a softcore one too).  From this console's unlocks.bin (verified) and
// <rom>.unl, the account's unlocks as RetroAchievements listed them for RA
// Prep/Sync ("S <ids>" and "H <ids>" lines; editing it can only hide or
// repeat achievements, which RA then refuses as already unlocked).
static void collectDone(const char* md5, const char* name, RaBootHeader* boot, bool hardcore) {
	FILE* bin = fopen(RA_UNLOCKS, "rb");
	if (bin) {
		RaSignedUnlock u;
		while (fread(&u, sizeof(u), 1, bin) == 1) {
			if (strncmp(u.record.md5, md5, 32) == 0 && (!hardcore || (u.record.rtc[7] & 1)) && verifyUnlock(u)) {
				addDone(boot, u.record.achievementId);
			}
		}
		fclose(bin);
	}
	FILE* unl = fopen((std::string(RA_SETS) + name + ".unl").c_str(), "rb");
	if (unl) {
		char line[4096];
		while (fgets(line, sizeof(line), unl)) {
			if ((line[0] == 'H' || (line[0] == 'S' && !hardcore)) && line[1] == ' ') {
				for (char* p = line + 2; *p;) {
					char* end;
					const u32 id = strtoul(p, &end, 10);
					if (end == p) break;
					if (id) addDone(boot, id);
					p = end;
				}
			}
		}
		fclose(unl);
	}
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
//   hardcore=1|0                hardcore until chosen in the in-game menu (0)
static u32 readConfig(void) {
	u32 config = RA_CFG_DEFAULT;
	FILE* f = fopen(RA_DIR "/config.txt", "rb");
	char line[128];
	while (f && fgets(line, sizeof(line), f)) {
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
		} else if (strcmp(line, "hardcore") == 0) {
			config = (strcmp(value, "1") == 0) ? (config | RA_CFG_HARDCORE) : (config & ~RA_CFG_HARDCORE);
		}
	}
	if (f) {
		fclose(f);
	}
	// Softcore/hardcore chosen in the in-game menu wins over config.txt
	FILE* dump = fopen("sd:/_nds/nds-bootstrap/ramDump.bin", "rb");
	if (dump) {
		u32 mode[2] = {0, 0};
		if (fseek(dump, RA_DUMP_MODE_OFFSET, SEEK_SET) == 0 && fread(mode, sizeof(mode), 1, dump) == 1
		 && mode[0] == RA_MODE_MAGIC) {
			config = mode[1] ? (config | RA_CFG_HARDCORE) : (config & ~RA_CFG_HARDCORE);
		}
		fclose(dump);
	}
	#if !RA_HARDCORE_AVAILABLE
	config &= ~RA_CFG_HARDCORE; // not yet (ra_engine.h)
	#endif
	// Hardcore evaluates every frame
	if (config & RA_CFG_HARDCORE) {
		config &= ~(0xFFu << RA_CFG_INTERVAL_SHIFT);
	}
	return config;
}

// Hardcore for this game: config.txt says so and the game has a set.  The
// loader then leaves the cheats out; the card engine and in-game menu block
// the RAM viewer/editor and refresh-rate changes (RaBootHeader.config).
bool raHardcoreGame(const configuration* conf) {
	if (!conf->ndsPath || strncmp(conf->ndsPath, "sd:/", 4) != 0 || !(readConfig() & RA_CFG_HARDCORE)) {
		return false;
	}
	const char* name = strrchr(conf->ndsPath, '/');
	name = name ? name + 1 : conf->ndsPath;
	struct stat st;
	return stat((std::string(RA_DIR "/sets/") + name + ".txt").c_str(), &st) == 0;
}

// sd:/_nds/ra/loader_status.txt: what this start found, for troubleshooting
// (on a 3DS especially).  The key id is RA Tool's: both must show the same.
static void writeStatus(const configuration* conf, bool keyed, bool haveSet, bool setSigned) {
	FILE* f = fopen(RA_DIR "/loader_status.txt", "wb");
	if (!f) {
		return;
	}
	char keyId[9] = "none";
	if (keyed) {
		static const char label[] = "RA-NDS key id";
		u8 mac[32];
		raHmacSha256(raKey, label, sizeof(label) - 1, mac);
		snprintf(keyId, sizeof(keyId), "%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3]);
	}
	const char* name = strrchr(conf->ndsPath, '/');
	fprintf(f, "game %s\nconsole model %d\nconsole key %s\nset %s\nconfig %08lx\n",
		name ? name + 1 : conf->ndsPath, conf->consoleModel, keyId,
		!haveSet ? "missing" : setSigned ? "loaded" : "not signed by this console (run RA Tool / RA Prep)",
		(unsigned long)readConfig());
	fclose(f);
}

void raPrepareBoot(const configuration* conf, const std::string& ramDumpPath) {
	if (!dsiFeatures() || conf->b4dsMode || conf->bootstrapOnFlashcard || conf->gameOnFlashcard) {
		return;
	}
	// No console key (no eMMC CID): unlocks couldn't be signed, so no RA
	const bool keyed = deriveKey();
	FILE* dump = fopen(ramDumpPath.c_str(), "r+b");
	if (!dump) {
		return;
	}

	RaBootHeader* boot = (RaBootHeader*)calloc(1, RA_ENGINE_OFFSET);
	if (keyed) {
		memcpy(boot->cid, raCid, sizeof(boot->cid)); // the engine derives the key from it
	}
	boot->unlockSeq = flushUnlocks(dump);

	// sd:/_nds/ra/sets/<ROM file name>.txt, written and signed by RA Prep
	const char* name = strrchr(conf->ndsPath, '/');
	name = name ? name + 1 : conf->ndsPath;
	std::string setPath = std::string(RA_DIR "/sets/") + name + ".txt";

	std::vector<u8> set, engine;
	const bool haveSet = keyed && readFile(setPath.c_str(), set, RA_SET_MAX - 1);
	const bool setSigned = haveSet && setSignatureOk(name, set);
	writeStatus(conf, keyed, haveSet, setSigned);
	if (setSigned && readFile("nitro:/cardenginei_arm7_ra.bin", engine, RA_ENGINE_MAX)) {
		boot->config = readConfig();
		// "game\t<id>\t<md5>\t<title>" is the second line
		set.push_back(0); // for strstr; not written out
		const char* game = strstr((const char*)set.data(), "\ngame\t");
		const char* md5 = game ? strchr(game + 6, '\t') : NULL;
		if (md5) {
			collectDone(md5 + 1, name, boot, boot->config & RA_CFG_HARDCORE);
		}
		set.pop_back();
		boot->magic = RA_BOOT_MAGIC;
		boot->engineSize = engine.size();
		boot->setSize = set.size();

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
	// Not on a 3DS (console model 2 and up): no Unlaunch to restart through.
	// There RA Tool prepares the sets.
	if (!conf->ndsPath || strncmp(conf->ndsPath, "sd:/", 4) != 0 || !isDSiMode() || conf->consoleModel >= 2) {
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
	// A set counts only with a good signature (RA Prep signs what it fetches)
	std::vector<u8> set;
	const bool setOk = readFile((setPath + ".txt").c_str(), set, RA_SET_MAX - 1) && setSignatureOk(name, set);
	if ((setOk || stat((setPath + ".none").c_str(), &st) == 0)
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
	// Not on a 3DS either (RA Sync returns through Unlaunch); there RA Tool
	// sends the unlocks
	if (!conf->ndsPath || strncmp(conf->ndsPath, "sd:/", 4) != 0 || conf->consoleModel >= 2
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
	// The quit target, then the game (RA Sync refreshes its account unlocks)
	fprintf(f, "%s\n%s\n", conf->quitPath, conf->ndsPath);
	fclose(f);
	conf->quitPath = strdup(RA_SYNC_PATH);
}

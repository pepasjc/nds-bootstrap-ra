/*
	RetroAchievements engine for the ARM7 card engine.

	Loaded into RA_REGION with the game's achievement set.  The card engine
	calls init() once and frame() every VBlank (through ra_entry.s, which
	switches to this region's own stack).  Achievements are parsed a few per
	frame, then every loaded trigger is evaluated against main RAM.
*/

#include <string.h>
#include <stdlib.h>
#include <nds/ndstypes.h>

#include "rc_runtime_types.h"
#include "ra_engine.h"

#define RA_MAX_ACHIEVEMENTS 512
#define PARSE_PER_FRAME 8
#define REG_VCOUNT (*(vu16*)0x04000006)

extern int raInitStub(const struct RaHost* host);
extern void raFrameStub(void);

struct RaStats raStats;

struct RaEngineHeader raEngineHeader __attribute__((section(".raheader"), used)) = {
	RA_ENGINE_MAGIC,
	1,
	raInitStub,
	raFrameStub,
	&raStats,
	0,
	{0},
};

struct Achievement {
	u32 id;
	u32 points;
	const char* title;
	const char* memaddr;
	rc_trigger_t* trigger;
};

static struct Achievement achievements[RA_MAX_ACHIEVEMENTS];
static u32 achievementCount;
static const struct RaHost* host;

// ---------------------------------------------------------------------------
// Heap: rcheevos only allocates while parsing, so a bump allocator will do.

static u8* heapPtr = (u8*)(RA_REGION + RA_HEAP_OFFSET);
static u8* const heapEnd = (u8*)(RA_REGION + RA_REGION_SIZE - RA_STACK_SIZE);

void* malloc(size_t size) {
	size = (size + 7) & ~7;
	if (heapPtr + size > heapEnd) {
		return NULL;
	}
	void* p = heapPtr;
	heapPtr += size;
	raStats.heapUsed = heapPtr - (u8*)(RA_REGION + RA_HEAP_OFFSET);
	return p;
}

void free(void* p) {
	(void)p;
}

void* calloc(size_t count, size_t size) {
	void* p = malloc(count * size);
	if (p) {
		memset(p, 0, count * size);
	}
	return p;
}

void* realloc(void* old, size_t size) {
	void* p = malloc(size);
	if (p && old) {
		// The old block's size is unknown; it lies below p in the same heap.
		size_t avail = (u8*)p - (u8*)old;
		memcpy(p, old, avail < size ? avail : size);
	}
	return p;
}

// ---------------------------------------------------------------------------

static uint32_t peek(uint32_t address, uint32_t numBytes, void* ud) {
	(void)ud;
	// $000000-$3FFFFF is main RAM; the DSi-only and Data TCM ranges read 0.
	if (address + numBytes > 0x400000) {
		return 0;
	}
	const vu8* p = (const vu8*)(0x02000000 + address);
	switch (numBytes) {
		case 1:
			return p[0];
		case 2:
			return p[0] | (p[1] << 8);
		case 4:
			return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
	}
	return 0;
}

static bool isDone(u32 id) {
	const struct RaBootHeader* boot = host->boot;
	for (u32 i = 0; i < boot->doneCount && i < RA_MAX_DONE; i++) {
		if (boot->done[i] == id) {
			return true;
		}
	}
	return false;
}

static u32 parseUnsigned(const char* s) {
	u32 v = 0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s++ - '0');
	}
	return v;
}

// Split the tab-separated line at *cursor into fields, in place.
static int splitLine(char** cursor, char* end, char** fields, int maxFields) {
	char* p = *cursor;
	int n = 0;
	fields[n++] = p;
	while (p < end && *p != '\n') {
		if (*p == '\t' && n < maxFields) {
			*p = 0;
			fields[n++] = p + 1;
		} else if (*p == '\r') {
			*p = 0;
		}
		p++;
	}
	if (p < end) {
		*p++ = 0;
	}
	*cursor = p;
	return n;
}

static void copyMd5(const char* src) {
	int i = 0;
	for (; i < 32 && src[i]; i++) {
		raEngineHeader.md5[i] = src[i];
	}
	raEngineHeader.md5[i] = 0;
}

extern char __bss_start[], __bss_end[];

int raInit(const struct RaHost* h) {
	// Only code and data are loaded; the rest of the region is stale RAM.
	memset(__bss_start, 0, __bss_end - __bss_start);
	host = h;
	achievementCount = 0;

	char* cursor = host->set;
	char* end = host->set + host->setSize;
	char* fields[5];
	while (cursor < end) {
		int n = splitLine(&cursor, end, fields, 5);
		if (n >= 4 && strcmp(fields[0], "game") == 0) {
			raEngineHeader.gameId = parseUnsigned(fields[1]);
			copyMd5(fields[2]);
		} else if (n >= 5 && strcmp(fields[0], "ach") == 0) {
			u32 id = parseUnsigned(fields[1]);
			if (isDone(id) || achievementCount >= RA_MAX_ACHIEVEMENTS) {
				continue;
			}
			struct Achievement* a = &achievements[achievementCount++];
			a->id = id;
			a->points = parseUnsigned(fields[2]);
			a->memaddr = fields[3];
			a->title = fields[4];
			a->trigger = NULL;
		}
	}
	raStats.achievements = achievementCount;
	return achievementCount;
}

static u32 parsedCount;

static void parseSome(void) {
	for (int i = 0; i < PARSE_PER_FRAME && parsedCount < achievementCount; i++) {
		struct Achievement* a = &achievements[parsedCount++];
		int size = rc_trigger_size(a->memaddr);
		void* buffer = size > 0 ? malloc(size) : NULL;
		a->trigger = buffer ? rc_parse_trigger(buffer, a->memaddr, NULL, 0) : NULL;
		if (a->trigger) {
			raStats.parsed++;
		} else {
			raStats.parseErrors++;
		}
	}
}

void raFrame(void) {
	const u16 start = REG_VCOUNT;
	raStats.frames++;

	if (parsedCount < achievementCount) {
		parseSome();
	}

	for (u32 i = 0; i < parsedCount; i++) {
		struct Achievement* a = &achievements[i];
		if (!a->trigger) {
			continue;
		}
		if (rc_evaluate_trigger(a->trigger, peek, NULL, NULL) == RC_TRIGGER_STATE_TRIGGERED) {
			raStats.unlocks++;
			host->unlocked(a->id, a->points, a->title);
		}
	}

	const u16 now = REG_VCOUNT;
	const u32 lines = (now >= start) ? (u32)(now - start) : (u32)(now + 263 - start);
	raStats.lastLines = lines;
	if (lines > raStats.maxLines) {
		raStats.maxLines = lines;
	}
}

/*
	RetroAchievements engine for the ARM7 card engine.

	Loaded into RA_REGION with the game's achievement set.  The card engine
	calls init() once and frame() every VBlank (through ra_entry.s, which
	switches to this region's own stack).  Achievements are activated in an
	rc_runtime a few per frame; the runtime shares memory references between
	them, so each address is read once a frame however many use it.
*/

#include <string.h>
#include <stdlib.h>
#include <nds/ndstypes.h>

#include "rc_runtime.h"
#include "ra_engine.h"
#include "ra_fast.h"

#define RA_MAX_ACHIEVEMENTS 512
#define PARSE_PER_FRAME 8
#define REG_VCOUNT (*(vu16*)0x04000006)
#define GAME_RAM ((const vu8*)0x02000000)
#define GAME_RAM_SIZE 0x400000

extern int raInitStub(const struct RaHost* host);
extern void raFrameStub(void);

// The ARM9 achievements list and the card engine read these, so they stay in
// RA_REGION even when the rest of .bss is in the ARM7's WRAM (see the
// linker scripts)
#define MAIN_RAM __attribute__((section(".mainbss")))
struct RaStats raStats MAIN_RAM;
static struct RaAchievement achievements[RA_MAX_ACHIEVEMENTS] MAIN_RAM;
static rc_runtime_t runtime;

struct RaEngineHeader raEngineHeader __attribute__((section(".raheader"), used)) = {
	RA_ENGINE_MAGIC,
	1,
	raInitStub,
	raFrameStub,
	&raStats,
	0,
	{0},
	{0},
	achievements,
	0,
};

static u32 achievementCount;
static const struct RaHost* host;

// ---------------------------------------------------------------------------
// Heap.  rcheevos allocates while activating achievements: the kept
// trigger buffers plus scratch space it frees straight away, newest first.
// So: a bump allocator whose top blocks are given back once freed.

struct Block {
	struct Block* prev;
	u32 size;  // payload bytes
	u32 freed;
	u32 pad;
};

static u8* const heapStart = (u8*)(RA_REGION + RA_HEAP_OFFSET);
static u8* const heapEnd = (u8*)(RA_REGION + RA_REGION_SIZE - RA_STACK_SIZE);
static u8* heapPtr;
static struct Block* heapTop;

static void heapInit(void) {
	heapPtr = heapStart;
	heapTop = NULL;
}

void* malloc(size_t size) {
	size = (size + 7) & ~7;
	if (heapPtr + sizeof(struct Block) + size > heapEnd) {
		return NULL;
	}
	struct Block* b = (struct Block*)heapPtr;
	b->prev = heapTop;
	b->size = size;
	b->freed = 0;
	heapTop = b;
	heapPtr += sizeof(struct Block) + size;
	if ((u32)(heapPtr - heapStart) > raStats.heapUsed) {
		raStats.heapUsed = heapPtr - heapStart;
	}
	return b + 1;
}

void free(void* p) {
	if (!p) {
		return;
	}
	((struct Block*)p - 1)->freed = 1;
	while (heapTop && heapTop->freed) {
		heapPtr = (u8*)heapTop;
		heapTop = heapTop->prev;
	}
}

void* calloc(size_t count, size_t size) {
	void* p = malloc(count * size);
	if (p) {
		memset(p, 0, count * size);
	}
	return p;
}

void* realloc(void* old, size_t size) {
	if (!old) {
		return malloc(size);
	}
	struct Block* b = (struct Block*)old - 1;
	size = (size + 7) & ~7;
	if (b == heapTop && (u8*)old + size <= heapEnd) {
		// Newest block: grow in place
		b->size = size;
		heapPtr = (u8*)old + size;
		return old;
	}
	void* p = malloc(size);
	if (p) {
		memcpy(p, old, b->size < size ? b->size : size);
		free(old);
	}
	return p;
}

// ---------------------------------------------------------------------------

static uint32_t peek(uint32_t address, uint32_t numBytes, void* ud) {
	(void)ud;
	// $000000-$3FFFFF is main RAM; the DSi-only and Data TCM ranges read 0.
	if (address >= GAME_RAM_SIZE || GAME_RAM_SIZE - address < numBytes) {
		return 0;
	}
	const vu8* p = GAME_RAM + address;
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

static void copyString(char* dst, const char* src, int max) {
	int i = 0;
	for (; i < max - 1 && src[i]; i++) {
		dst[i] = src[i];
	}
	dst[i] = 0;
}

extern char __bss_start[], __bss_end[], __mainbss_start[], __mainbss_end[];

int raInit(const struct RaHost* h) {
	// Only code and data are loaded; the rest of the region is stale RAM.
	memset(__bss_start, 0, __bss_end - __bss_start);
	memset(__mainbss_start, 0, __mainbss_end - __mainbss_start);
	heapInit();
	rc_runtime_init(&runtime);
	host = h;
	raFastPoll = h->poll;
	achievementCount = 0;

	char* cursor = host->set;
	char* end = host->set + host->setSize;
	char* fields[6];
	while (cursor < end) {
		int n = splitLine(&cursor, end, fields, 6);
		if (n >= 4 && strcmp(fields[0], "game") == 0) {
			raEngineHeader.gameId = parseUnsigned(fields[1]);
			copyString(raEngineHeader.md5, fields[2], sizeof(raEngineHeader.md5));
			copyString(raEngineHeader.title, fields[3], sizeof(raEngineHeader.title));
		} else if (n >= 5 && strcmp(fields[0], "ach") == 0 && achievementCount < RA_MAX_ACHIEVEMENTS) {
			struct RaAchievement* a = &achievements[achievementCount++];
			a->id = parseUnsigned(fields[1]);
			a->points = parseUnsigned(fields[2]);
			a->memaddr = fields[3];
			a->title = fields[4];
			a->description = (n >= 6) ? fields[5] : "";
			a->trigger = NULL;
			a->status = isDone(a->id) ? RA_UNLOCKED_BEFORE : RA_LOCKED;
		}
	}
	raEngineHeader.count = achievementCount;
	raStats.achievements = achievementCount;
	return achievementCount;
}

static u32 parsedCount;

// Achievements are activated a few per frame and evaluated by rcheevos;
// once all are, ra_fast.c takes over (rcheevos again if out of memory).
enum { FAST_OFF, FAST_ON, FAST_FAILED };
static u32 fastState;

static struct RaAchievement* findAchievement(u32 id) {
	for (u32 i = 0; i < achievementCount; i++) {
		if (achievements[i].id == id) {
			return &achievements[i];
		}
	}
	return NULL;
}

static void activateSome(void) {
	for (int i = 0; i < PARSE_PER_FRAME && parsedCount < achievementCount; i++) {
		if (host->poll) {
			host->poll();
		}
		struct RaAchievement* a = &achievements[parsedCount++];
		if (a->status != RA_LOCKED) {
			continue;
		}
		if (rc_runtime_activate_achievement(&runtime, a->id, a->memaddr, NULL, 0) == RC_OK) {
			a->trigger = rc_runtime_get_achievement(&runtime, a->id);
			raStats.parsed++;
		} else {
			a->status = RA_UNSUPPORTED;
			raStats.parseErrors++;
		}
	}
}

static void onEvent(const rc_runtime_event_t* event) {
	if (event->type != RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
		return;
	}
	// Not deactivated: a triggered achievement is never evaluated again, and
	// ra_fast.c keeps pointers into it.
	struct RaAchievement* a = findAchievement(event->id);
	if (a && a->status == RA_LOCKED) {
		a->status = RA_UNLOCKED_NOW;
		raStats.unlocks++;
		host->unlocked(a->id, a->points, a->title);
	}
}

void raFrame(void) {
	const u16 start = REG_VCOUNT;
	raStats.frames++;

	if (fastState == FAST_OFF) {
		if (parsedCount < achievementCount) {
			activateSome();
		}
		rc_runtime_do_frame(&runtime, onEvent, peek, NULL, NULL);
		if (parsedCount == achievementCount) {
			fastState = raFastPrepare(&runtime) ? FAST_ON : FAST_FAILED;
		}
	} else if (fastState == FAST_ON) {
		raFastFrame(&runtime, onEvent, peek, NULL, (const u8*)GAME_RAM, GAME_RAM_SIZE);
	} else {
		rc_runtime_do_frame(&runtime, onEvent, peek, NULL, NULL);
	}

	const u16 now = REG_VCOUNT;
	const u32 lines = (now >= start) ? (u32)(now - start) : (u32)(now + 263 - start);
	raStats.lastLines = lines;
	if (lines > raStats.maxLines) {
		raStats.maxLines = lines;
	}
}

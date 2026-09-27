/*
	Host side of the ARM7 RetroAchievements engine, shared by ra_bench.c and
	ra_diff.c: loads a set (sd:/_nds/ra/sets format, or a random synthetic
	one), runs it the way the engine does, and generates RAM sequences.

	Engines, picked at build time:
	  RA_REF   stock rcheevos, the engine's old loop: rc_runtime_do_frame,
	           triggered achievements deactivated
	  RA_SC    same, with trigger.c patched to always short-circuit (the
	           engine before ra_fast.c)
	  default  the engine now: rc_runtime_do_frame while activating, then
	           ra_fast.c (trigger.c patched as in the engine Makefile)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rc_runtime.h"
#include "rcheevos/rc_internal.h"
#if !defined(RA_REF) && !defined(RA_SC)
#include "ra_fast.h"
#endif

#define RAM_SIZE 0x400000
#define MAX_ACH 1024
#define PARSE_PER_FRAME 8  // as in ra_engine.c

static unsigned char ram[RAM_SIZE];

static uint32_t peek(uint32_t address, uint32_t numBytes, void* ud) {
	(void)ud;
	if (address >= RAM_SIZE || RAM_SIZE - address < numBytes) return 0;
	const unsigned char* p = ram + address;
	switch (numBytes) {
		case 1: return p[0];
		case 2: return p[0] | (p[1] << 8);
		case 4: return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Random numbers (xorshift, deterministic per seed)

static uint64_t rng = 1;
static uint32_t rnd(void) {
	rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }
static int chance(uint32_t percent) { return below(1000) < percent * 10; }
static void seedRng(uint32_t seed) {
	rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0x2545F4914F6CDD1Dull);
	for (int i = 0; i < 20; i++) rnd();
}

// ---------------------------------------------------------------------------
// Sets

typedef struct { uint32_t id; const char* memaddr; } Ach;
static Ach achs[MAX_ACH];
static int achCount;
static char setText[0x80000];

static void loadSet(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) { perror(path); exit(2); }
	size_t n = fread(setText, 1, sizeof(setText) - 1, f);
	fclose(f);
	setText[n] = 0;
	for (char* line = strtok(setText, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		if (strncmp(line, "ach\t", 4) != 0 || achCount == MAX_ACH) continue;
		char* fields[6] = {0};
		int nf = 0;
		for (char* p = line; nf < 6;) {
			fields[nf++] = p;
			p = strchr(p, '\t');
			if (!p) break;
			*p++ = 0;
		}
		if (nf < 4) continue;
		achs[achCount].id = strtoul(fields[1], NULL, 10);
		achs[achCount].memaddr = fields[3];
		achCount++;
	}
}

// Synthetic sets: random conditions over 16 bytes at SYNTH_BASE, with small
// values so that they are often true.  Covers every flag rcheevos has; some
// strings won't parse and are skipped (by both engines alike).
#define SYNTH_BASE 0x100
#define SYNTH_SPAN 16

static char* synthOperand(char* p, int allowConst) {
	if (allowConst && chance(30)) {
		if (chance(5)) return p + sprintf(p, "f%d.5", below(3));
		return p + sprintf(p, "%d", below(5));
	}
	if (allowConst && chance(2)) return p + sprintf(p, "{recall}");
	static const char* prefixes[] = {"", "", "", "", "", "", "d", "d", "d", "p", "b", "~"};
	static const char* sizes[] = {"H", "H", "H", "H", "H", " ", "X", "L", "U", "M", "N", "O", "P", "Q", "R", "S", "T", "K", "I", "W", "G"};
	const char* size = sizes[below(sizeof(sizes) / sizeof(*sizes))];
	return p + sprintf(p, "%s0x%s%04x", prefixes[below(sizeof(prefixes) / sizeof(*prefixes))],
	                   size[0] == ' ' ? "" : size, SYNTH_BASE + below(SYNTH_SPAN));
}

static char* synthCondition(char* p, int hitless) {
	static const char* flags[] = {"", "", "", "", "", "", "", "", "", "", "R:", "R:", "R:", "P:", "P:", "N:", "N:",
	                              "N:", "O:", "O:", "A:", "A:", "B:", "B:", "C:", "D:", "Z:", "Z:", "T:", "M:",
	                              "Q:", "K:", "I:", "I:"};
	const char* flag;
	do {
		flag = flags[below(sizeof(flags) / sizeof(*flags))];
	} while (hitless && (flag[0] == 'P' || flag[0] == 'M' || flag[0] == 'Q'));
	p += sprintf(p, "%s", flag);
	p = synthOperand(p, 0);
	if (flag[0] == 'A' || flag[0] == 'B' || flag[0] == 'I' || flag[0] == 'K') {
		static const char* mods[] = {"*", "/", "&", "^", "%", "+", "-"};
		if (chance(30)) {
			p += sprintf(p, "%s", mods[below(7)]);
			p = synthOperand(p, 1);
		}
		return p;
	}
	static const char* ops[] = {"=", "!=", "<", "<=", ">", ">="};
	p += sprintf(p, "%s", ops[below(6)]);
	p = synthOperand(p, 1);
	if (!hitless && chance(30)) p += sprintf(p, ".%d.", 1 + below(6));
	return p;
}

static void synthSet(int count) {
	char* p = setText;
	for (int i = 0; i < count && i < MAX_ACH; i++) {
		const int hitless = chance(50);
		achs[i].id = 1000 + i;
		achs[i].memaddr = p;
		const int groups = chance(50) ? 1 : 2 + below(3);
		for (int g = 0; g < groups; g++) {
			if (g) *p++ = 'S';
			const int conds = 1 + below(g ? 4 : 7);
			for (int c = 0; c < conds; c++) {
				if (c) *p++ = '_';
				p = synthCondition(p, hitless);
			}
		}
		*p++ = 0;
		achCount = i + 1;
	}
}

// ---------------------------------------------------------------------------
// The engine's loop

static rc_runtime_t rt;
static int parsed, frameNo;
static int fastOn;
static uint32_t events[MAX_ACH];
static int eventCount;

static void onEvent(const rc_runtime_event_t* e) {
	if (e->type != RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) return;
	events[eventCount++] = e->id;
#ifdef RA_REF
	rc_runtime_deactivate_achievement(&rt, e->id);  // the old engine did
#endif
}

static void engineInit(void) {
	rc_runtime_init(&rt);
	parsed = frameNo = fastOn = 0;
}

// Returns the number of achievements that parsed
static int activateAll(void) {
	int ok = 0;
	for (parsed = 0; parsed < achCount; parsed++)
		ok += rc_runtime_activate_achievement(&rt, achs[parsed].id, achs[parsed].memaddr, NULL, 0) == RC_OK;
	return ok;
}

static void engineFrame(void) {
	eventCount = 0;
	frameNo++;
	if (!fastOn) {
		for (int i = 0; i < PARSE_PER_FRAME && parsed < achCount; i++, parsed++)
			rc_runtime_activate_achievement(&rt, achs[parsed].id, achs[parsed].memaddr, NULL, 0);
		rc_runtime_do_frame(&rt, onEvent, peek, NULL, NULL);
#if !defined(RA_REF) && !defined(RA_SC)
		if (parsed == achCount) {
			fastOn = raFastPrepare(&rt) ? 1 : -1;
		}
#endif
	} else if (fastOn > 0) {
#if !defined(RA_REF) && !defined(RA_SC)
		raFastFrame(&rt, onEvent, peek, NULL, ram, RAM_SIZE);
#endif
	} else {
		rc_runtime_do_frame(&rt, onEvent, peek, NULL, NULL);
	}
}

// ---------------------------------------------------------------------------
// RAM writes by operand size

static uint32_t ramRead(uint32_t a, uint8_t size) {
	return rc_peek_value(a, size, peek, NULL);
}

static void put8(uint32_t a, uint32_t v) { if (a < RAM_SIZE) ram[a] = (unsigned char)v; }

static void ramWrite(uint32_t a, uint8_t size, uint32_t v) {
	switch (size) {
		case RC_MEMSIZE_8_BITS: put8(a, v); break;
		case RC_MEMSIZE_16_BITS: put8(a, v); put8(a + 1, v >> 8); break;
		case RC_MEMSIZE_24_BITS: put8(a, v); put8(a + 1, v >> 8); put8(a + 2, v >> 16); break;
		case RC_MEMSIZE_LOW: if (a < RAM_SIZE) ram[a] = (ram[a] & 0xF0) | (v & 0x0F); break;
		case RC_MEMSIZE_HIGH: if (a < RAM_SIZE) ram[a] = (ram[a] & 0x0F) | ((v & 0x0F) << 4); break;
		case RC_MEMSIZE_BITCOUNT: put8(a, v >= 8 ? 0xFF : (1u << v) - 1); break;
		case RC_MEMSIZE_16_BITS_BE: put8(a, v >> 8); put8(a + 1, v); break;
		case RC_MEMSIZE_24_BITS_BE: put8(a, v >> 16); put8(a + 1, v >> 8); put8(a + 2, v); break;
		case RC_MEMSIZE_32_BITS_BE: put8(a, v >> 24); put8(a + 1, v >> 16); put8(a + 2, v >> 8); put8(a + 3, v); break;
		default:
			if (size >= RC_MEMSIZE_BIT_0 && size <= RC_MEMSIZE_BIT_7) {
				const int bit = size - RC_MEMSIZE_BIT_0;
				if (a < RAM_SIZE) ram[a] = (ram[a] & ~(1 << bit)) | ((v & 1) << bit);
			} else {
				put8(a, v); put8(a + 1, v >> 8); put8(a + 2, v >> 16); put8(a + 3, v >> 24);
			}
	}
}

// "Hints": memory = constant comparisons in the set, used to steer random
// RAM towards making conditions true.
typedef struct { uint32_t address, value; uint8_t size, oper, type; } Hint;
static Hint hints[16384];
static int hintCount;
typedef struct { int firstHint, hintCount, firstLeaf, leafCount; } AchHints;
static AchHints achHints[MAX_ACH];
static uint32_t leaves[16384];  // address << 8 | size
static int leafCount;

static void addLeaf(const rc_operand_t* op, int depth) {
	if (!rc_operand_is_memref(op) || !op->value.memref || depth > 64) return;
	if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MODIFIED_MEMREF) {
		const rc_modified_memref_t* m = (const rc_modified_memref_t*)op->value.memref;
		if (m->modifier_type != RC_OPERATOR_INDIRECT_READ) {
			addLeaf(&m->parent, depth + 1);
			addLeaf(&m->modifier, depth + 1);
		}
		return;
	}
	if (op->value.memref->value.memref_type == RC_MEMREF_TYPE_MEMREF && leafCount < 16384)
		leaves[leafCount++] = op->value.memref->address << 8 | op->size;
}

// Parse the set into a scratch runtime (same parser in every build) and
// collect hints per achievement
static void collectHints(void) {
	rc_runtime_t scratch;
	rc_runtime_init(&scratch);
	for (int i = 0; i < achCount; i++) {
		AchHints* h = &achHints[i];
		h->firstHint = hintCount;
		h->firstLeaf = leafCount;
		if (rc_runtime_activate_achievement(&scratch, achs[i].id, achs[i].memaddr, NULL, 0) == RC_OK) {
			rc_trigger_t* t = rc_runtime_get_achievement(&scratch, achs[i].id);
			for (rc_condset_t* s = t->requirement ? t->requirement : t->alternative; s;
			     s = (s == t->requirement) ? t->alternative : s->next) {
				for (rc_condition_t* c = s->conditions; c; c = c->next) {
					addLeaf(&c->operand1, 0);
					addLeaf(&c->operand2, 0);
					if (rc_operand_is_memref(&c->operand1) && c->operand1.value.memref &&
					    c->operand1.value.memref->value.memref_type == RC_MEMREF_TYPE_MEMREF &&
					    c->operand2.type == RC_OPERAND_CONST && c->oper <= RC_OPERATOR_NE && hintCount < 16384) {
						Hint* x = &hints[hintCount++];
						x->address = c->operand1.value.memref->address;
						x->size = c->operand1.size;
						x->oper = c->oper;
						x->type = c->operand1.type;
						x->value = c->operand2.value.num;
					}
				}
			}
		}
		h->hintCount = hintCount - h->firstHint;
		h->leafCount = leafCount - h->firstLeaf;
	}
	rc_runtime_destroy(&scratch);
}

static uint32_t toBcd(uint32_t v) {
	uint32_t r = 0;
	for (int s = 0; v && s < 32; s += 4, v /= 10) r |= (v % 10) << s;
	return r;
}

// Make a hint's condition true (or nearly)
static void applyHint(const Hint* h) {
	uint32_t v = h->value;
	switch (h->oper) {
		case RC_OPERATOR_LT: v = v ? v - 1 - below(v < 3 ? v : 3) : 0; break;
		case RC_OPERATOR_GT: v = v + 1 + below(3); break;
		case RC_OPERATOR_LE: case RC_OPERATOR_GE: v += (h->oper == RC_OPERATOR_GE) ? below(2) : 0; break;
		case RC_OPERATOR_NE: v = v + 1 + below(3); break;
	}
	if (h->type == RC_OPERAND_BCD) v = toBcd(v);
	if (h->type == RC_OPERAND_INVERTED) v = ~v;
	ramWrite(h->address, h->size, v);
}

static void nudge(uint32_t address, uint8_t size) {
	uint32_t v = ramRead(address, size);
	v += chance(70) ? 1 + below(4) : (uint32_t)-(int)(1 + below(2));
	ramWrite(address, size, v);
}

// ---------------------------------------------------------------------------
// RAM sequences.  All depend only on the seed and the frame number.

// "random": follows one achievement at a time, making its conditions true
// in random order, with noise from the rest of the set.
static int focus = -1, focusUntil;
static void stepRandom(void) {
	if (frameNo >= focusUntil || focus < 0) {
		focus = below(achCount);
		focusUntil = frameNo + 50 + below(600);
		const AchHints* h = &achHints[focus];
		for (int i = 0; i < h->hintCount; i++)
			if (chance(70)) applyHint(&hints[h->firstHint + i]);
	}
	const AchHints* h = &achHints[focus];
	if (h->hintCount && chance(50)) applyHint(&hints[h->firstHint + below(h->hintCount)]);
	if (h->hintCount && chance(20)) {
		const Hint* x = &hints[h->firstHint + below(h->hintCount)];
		nudge(x->address, x->size);
	}
	if (h->leafCount && chance(25)) {
		const uint32_t l = leaves[h->firstLeaf + below(h->leafCount)];
		if (chance(50)) nudge(l >> 8, l & 0xFF);
		else ramWrite(l >> 8, l & 0xFF, chance(60) ? 0xFFFFFFFF : 0);
	}
	if (hintCount && chance(20)) applyHint(&hints[below(hintCount)]);
	if (hintCount && chance(10)) {
		const Hint* x = &hints[below(hintCount)];
		ramWrite(x->address, x->size, rnd());
	}
	if (chance(1)) {
		for (int i = 0; i < 20 && leafCount; i++) {
			const uint32_t l = leaves[below(leafCount)];
			ramWrite(l >> 8, l & 0xFF, below(4));
		}
	}
}

// "synth": a few of the 16 bytes change each frame (some frames none)
static void stepSynth(void) {
	int n = below(4);
	for (int i = 0; i < n; i++) {
		const uint32_t a = SYNTH_BASE + below(SYNTH_SPAN);
		if (chance(10)) ram[a] = (unsigned char)rnd();
		else if (chance(30)) ram[a] += chance(50) ? 1 : -1;
		else ram[a] = below(4);
	}
}

// "gameplay": Tetris DS Standard - Marathon.  Menus/mode bytes fixed on
// Marathon, pieces falling and rotating, lines/score/level going up,
// board filling; plus a little noise from the seed if noise is set.
static uint32_t lines;
static void stepGameplay(int noise) {
	const int f = frameNo;
	ram[0x076a2c] = 2;
	ram[0x07b3b0] = ram[0x07b3b1] = ram[0x07b3b2] = 0;
	ram[0x17dd1c] = (f / 45) % 7;                 // piece
	if (f % 9 == 0) ram[0x17dd1d] = (ram[0x17dd1d] + 1) & 3;  // rotation
	ram[0x17dd22] = (f / 45) % 20;                // row
	ram[0x17dd32] = 1 + (f / 45) % 5;
	if (f % 45 == 44) {                           // piece lands
		const uint32_t cleared = (f / 45) % 5 == 4 ? 1 + (f / 225) % 4 : 0;
		lines += cleared;
		ramWrite(0x17dd38, RC_MEMSIZE_32_BITS, lines);
		ramWrite(0x17dd34, RC_MEMSIZE_32_BITS, ramRead(0x17dd34, RC_MEMSIZE_32_BITS) + 10 + 100 * cleared);
		ramWrite(0x17dd3c, RC_MEMSIZE_32_BITS, 1 + lines / 10);
		ram[0x17db88 + (f / 45) % 110] = 0x11 * (1 + (f / 45) % 7);  // board cells
		ram[0x17ddc4] = (f / 45) % 3 == 0;
	}
	ram[0x17de87] = (f / 60) & 1;
	ram[0x1c4214] = 0;
	if (noise) {
		if (chance(5)) ram[0x17dd1c] = below(7);
		if (hintCount && chance(3)) applyHint(&hints[below(hintCount)]);
		if (hintCount && chance(2)) {
			const Hint* x = &hints[below(hintCount)];
			nudge(x->address, x->size);
		}
		if (chance(1)) ram[0x07b3b1] = below(6);  // brief visit to another mode
	}
}

/*
	A faster rc_runtime_do_frame for the ARM7 (see ra_fast.h).

	Per frame rcheevos updates every memory reference, then fully evaluates
	every achievement.  Three things make that cheaper here, each of them
	unable to change when an achievement triggers
	(tools/ra_diff.sh checks this against stock rcheevos):

	1. Gates.  Most achievements start with conditions like "game mode = 2"
	   that are false nearly all the time.  If an achievement has no hit
	   targets, PauseIf, Measured or MeasuredIf anywhere, a frame on which
	   one of its core group's plain conditions is false changes nothing that
	   matters in rcheevos: the achievement cannot trigger (the core group is
	   false), and the only state left behind is
	     - state ACTIVE: not paused (no PauseIf), not primed (a core
	       condition is false), and was_reset + has_hits keeps the old state,
	       which can't be WAITING (has_hits is always 0 in WAITING) or
	       PAUSED, and turns PRIMED into ACTIVE;
	     - hit counts of conditions without a hit target, has_hits and the
	       RESET notification, none of which ever decides whether a trigger
	       without hit targets is true.
	   So that frame is replaced by "state = ACTIVE".  A gate is a STANDARD
	   condition of the core group without a hit target that no OrNext feeds,
	   even through a Remember (AndNext and ResetNextIf can only make it
	   falser; AddHits needs a hit target to matter), and whose operands
	   are not {recall}.  Gates are shared between achievements and tested at
	   most once a frame.

	2. On-demand modified memrefs.  AddSource/SubSource chains become
	   "modified memrefs" that rcheevos recomputes every frame.  One that is
	   only ever read as a current value (never as delta/prior, not an
	   AddAddress read, not feeding one that is updated every frame) is a
	   function of this frame's memory alone, so it is computed only when an
	   achievement that uses it is about to be tested, once per frame.

	3. Short circuit.  With can_short_curcuit, rc_test_condset stops a
	   group's ResetIf pass at the first true ResetIf (all hits are reset
	   anyway) and skips / stops the "other" pass once the group is false.
	   That other pass may still hold AndNext/OrNext/ResetNextIf conditions
	   with hit targets, whose counts would then go stale, so achievements
	   with those are evaluated without it.

	Plain 8/16/32-bit memory references are also read straight from RAM.
*/

#include <stdlib.h>
#include <string.h>

#include "ra_fast.h"
#include "rc_internal.h"

// Defined by the engine's patched trigger.c (see the Makefile)
extern uint8_t ra_short_circuit;

typedef struct {
	rc_condition_t* cond;  // in a trigger that stays allocated
	uint32_t stamp;        // frame the result is for
	uint32_t result;
} Gate;

typedef struct {
	rc_modified_memref_t* memref;
	uint32_t stamp;        // frame the value is for
} Lazy;

typedef struct {
	rc_trigger_t* trigger; // NULL once triggered
	uint32_t id;
	// Counted lists: plain gates, lazy memrefs to compute, gates on those
	const uint16_t* prog;
	uint8_t shortCircuit;
} Entry;

static Entry* entries;
static Gate* gates;
static Lazy* lazies;
static rc_modified_memref_t** always;
static uint32_t entryCount, gateCount, lazyCount, alwaysCount, skippableCount, blockBytes;
static uint32_t frame;

// ---------------------------------------------------------------------------
// Frame

static int gateTrue(uint32_t i) {
	Gate* g = &gates[i];
	if (g->stamp != frame) {
		g->stamp = frame;
		g->result = rc_test_condition(g->cond, NULL);
	}
	return g->result;
}

static void updateMemrefs(rc_memrefs_t* memrefs, rc_runtime_peek_t peek, void* ud,
                          const uint8_t* ram, uint32_t ramSize) {
	rc_memref_list_t* list;
	for (list = &memrefs->memrefs; list; list = list->next) {
		rc_memref_t* m = list->items;
		rc_memref_t* end = m + list->count;
		for (; m < end; ++m) {
			const uint32_t a = m->address;
			uint32_t v;
			if (m->value.type == RC_VALUE_TYPE_NONE) {
				continue;
			}
			switch (m->value.size) {
				case RC_MEMSIZE_8_BITS:
					v = (a < ramSize) ? ram[a] : 0;
					break;
				case RC_MEMSIZE_16_BITS:
					if (a >= ramSize || ramSize - a < 2) {
						v = 0;
					} else if (a & 1) {
						v = ram[a] | (ram[a + 1] << 8);
					} else {
						v = *(const uint16_t*)(ram + a);
					}
					break;
				case RC_MEMSIZE_32_BITS:
					if (a >= ramSize || ramSize - a < 4) {
						v = 0;
					} else if (a & 3) {
						v = ram[a] | (ram[a + 1] << 8) | (ram[a + 2] << 16) | ((uint32_t)ram[a + 3] << 24);
					} else {
						v = *(const uint32_t*)(ram + a);
					}
					break;
				default:
					v = rc_peek_value(a, m->value.size, peek, ud);
					break;
			}
			// rc_update_memref_value
			if (m->value.value == v) {
				m->value.changed = 0;
			} else {
				m->value.prior = m->value.value;
				m->value.value = v;
				m->value.changed = 1;
			}
		}
	}
}

static void updateModified(rc_modified_memref_t* m, rc_runtime_peek_t peek, void* ud) {
	rc_update_memref_value(&m->memref.value, rc_get_modified_memref_value(m, peek, ud));
}

// Set by ra_engine.c: lets the card engine serve ROM reads mid-frame
void (*raFastPoll)(void);

void raFastFrame(rc_runtime_t* runtime, rc_runtime_event_handler_t handler,
                 rc_runtime_peek_t peek, void* ud, const uint8_t* ram, uint32_t ramSize) {
	uint32_t i, n;

	if (++frame == 0) {
		frame = 1;
	}
	updateMemrefs(runtime->memrefs, peek, ud, ram, ramSize);
	for (i = 0; i < alwaysCount; i++) {
		updateModified(always[i], peek, ud);
	}

	// Same order as rc_runtime_do_frame
	for (i = entryCount; i-- > 0;) {
		Entry* e = &entries[i];
		rc_trigger_t* t = e->trigger;
		const uint16_t* p = e->prog;
		if ((i & 7) == 0 && raFastPoll) {
			raFastPoll();
		}
		if (!t) {
			continue;
		}
		for (n = *p++; n; n--) {
			if (!gateTrue(*p++)) {
				goto skip;
			}
		}
		for (n = *p++; n; n--) {
			Lazy* l = &lazies[*p++];
			if (l->stamp != frame) {
				l->stamp = frame;
				updateModified(l->memref, peek, ud);
			}
		}
		for (n = *p++; n; n--) {
			if (!gateTrue(*p++)) {
				goto skip;
			}
		}

		ra_short_circuit = e->shortCircuit;
		if (rc_evaluate_trigger(t, peek, ud, NULL) == RC_TRIGGER_STATE_TRIGGERED) {
			rc_runtime_event_t event;
			e->trigger = NULL;
			event.id = e->id;
			event.value = 0;
			event.type = RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED;
			handler(&event);
		}
		continue;

	skip:
		// What a false frame leaves behind (see the top of the file)
		t->state = RC_TRIGGER_STATE_ACTIVE;
	}
}

// ---------------------------------------------------------------------------
// Prepare: two passes over the achievements, the first to size one block,
// the second to fill it.  Scratch memory is allocated after that block and
// freed, so the engine's heap gets it back.

#define ALWAYS 1  // modified memref updated every frame
#define MARK 2    // modified memref used by the achievement being prepared

static rc_memrefs_t* rtMemrefs;
static rc_modified_memref_t** mods;  // every modified memref, in update order
static uint32_t modCount;
static uint8_t* modFlags;
static uint16_t* lazyIndex;          // mods index -> lazies index
static rc_condition_t** reps;        // one condition per distinct gate
static uint16_t* hashNext;
static uint16_t hashHead[64];

static rc_condset_t* nextSet(const rc_trigger_t* t, const rc_condset_t* s) {
	if (!s) {
		return t->requirement ? t->requirement : t->alternative;
	}
	return (s == t->requirement) ? t->alternative : s->next;
}

static uint32_t otherStart(const rc_condset_t* s) {
	return s->num_pause_conditions + s->num_reset_conditions + s->num_hittarget_conditions +
	       s->num_measured_conditions;
}

static uint32_t condCount(const rc_condset_t* s) {
	return rc_condset_get_conditions((rc_condset_t*)s) ?
	       otherStart(s) + s->num_other_conditions + s->num_indirect_conditions : 0;
}

// Index in mods[] of the modified memref an operand reads, or -1.
// *history: the operand needs its delta/prior (a {recall}: assume so).
static int32_t modRef(const rc_operand_t* op, uint8_t* history) {
	const rc_memref_t* m;
	const rc_modified_memref_list_t* list;
	int32_t base = 0;

	if (op->type == RC_OPERAND_RECALL ? !rc_operand_type_is_memref(op->memref_access_type)
	                                  : !rc_operand_is_memref(op)) {
		return -1;
	}
	m = op->value.memref;
	if (!m || m->value.memref_type != RC_MEMREF_TYPE_MODIFIED_MEMREF) {
		return -1;
	}
	*history = !(op->type == RC_OPERAND_ADDRESS || op->type == RC_OPERAND_BCD ||
	             op->type == RC_OPERAND_INVERTED);
	for (list = &rtMemrefs->modified_memrefs; list; list = list->next) {
		const rc_modified_memref_t* mm = (const rc_modified_memref_t*)m;
		if (mm >= list->items && mm < list->items + list->count) {
			return base + (int32_t)(mm - list->items);
		}
		base += list->count;
	}
	return -1;
}

static void flagHistory(const rc_operand_t* op) {
	uint8_t history;
	const int32_t i = modRef(op, &history);
	if (i >= 0 && history) {
		modFlags[i] |= ALWAYS;
	}
}

static int isLazy(const rc_operand_t* op) {
	uint8_t history;
	const int32_t i = modRef(op, &history);
	return i >= 0 && !(modFlags[i] & ALWAYS);
}

// Mark the on-demand memrefs an operand needs, parents included
static void mark(const rc_operand_t* op) {
	uint8_t history;
	const int32_t i = modRef(op, &history);
	if (i >= 0 && !(modFlags[i] & (ALWAYS | MARK))) {
		modFlags[i] |= MARK;
		mark(&mods[i]->parent);
		mark(&mods[i]->modifier);
	}
}

static uint16_t* prog;  // NULL on the sizing pass
static uint32_t progLen;

static void put(uint32_t v) {
	if (prog) {
		prog[progLen] = v;
	}
	progLen++;
}

static int sameOperand(const rc_operand_t* a, const rc_operand_t* b) {
	if (a->type != b->type || a->size != b->size || a->memref_access_type != b->memref_access_type) {
		return 0;
	}
	if (rc_operand_is_memref(a)) {
		return a->value.memref == b->value.memref;
	}
	if (a->type == RC_OPERAND_FP) {
		return a->value.dbl == b->value.dbl;
	}
	return a->value.num == b->value.num;
}

static uint32_t operandKey(const rc_operand_t* op) {
	return rc_operand_is_memref(op) ? (uint32_t)(uintptr_t)op->value.memref : op->value.num;
}

static uint32_t gateIndex(rc_condition_t* c) {
	uint32_t h = (operandKey(&c->operand1) * 31 + operandKey(&c->operand2) + c->oper) % 64;
	uint32_t i;
	for (i = hashHead[h]; i != 0xFFFF; i = hashNext[i]) {
		const rc_condition_t* r = reps[i];
		if (r->oper == c->oper && r->optimized_comparator == c->optimized_comparator &&
		    sameOperand(&r->operand1, &c->operand1) && sameOperand(&r->operand2, &c->operand2)) {
			return i;
		}
	}
	reps[gateCount] = c;
	hashNext[gateCount] = hashHead[h];
	hashHead[h] = gateCount;
	return gateCount++;
}

// Whether an OrNext feeds conditions[k].  rc_test_condset_internal skips
// Remember/AddSource/SubSource/AddAddress without touching the OrNext flag,
// so "O:a_K:b_c" is c OR a.
static int orNextFeeds(const rc_condition_t* c, uint32_t k) {
	while (k-- > 0) {
		switch (c[k].type) {
			case RC_CONDITION_REMEMBER:
			case RC_CONDITION_ADD_SOURCE:
			case RC_CONDITION_SUB_SOURCE:
			case RC_CONDITION_ADD_ADDRESS:
				continue;
			default:
				return c[k].type == RC_CONDITION_OR_NEXT;
		}
	}
	return 0;
}

// Gates of the core group, those that read on-demand memrefs or the others
static void putGates(rc_trigger_t* t, int skippable, int lazy) {
	const uint32_t slot = progLen;
	uint32_t k, n = 0;
	put(0);
	if (skippable) {
		rc_condition_t* c = rc_condset_get_conditions(t->requirement);
		const uint32_t count = condCount(t->requirement);
		for (k = 0; k < count; k++) {
			if (c[k].type == RC_CONDITION_STANDARD && !c[k].required_hits && !orNextFeeds(c, k) &&
			    c[k].operand1.type != RC_OPERAND_RECALL && c[k].operand2.type != RC_OPERAND_RECALL &&
			    (isLazy(&c[k].operand1) || isLazy(&c[k].operand2)) == lazy) {
				put(gateIndex(&c[k]));
				n++;
			}
		}
	}
	if (prog) {
		prog[slot] = n;
	}
}

static int usable(const rc_runtime_trigger_t* rt) {
	return rt->trigger && !rt->invalid_memref && rc_trigger_state_active(rt->trigger->state);
}

int raFastPrepare(rc_runtime_t* runtime) {
	const rc_modified_memref_list_t* list;
	uint8_t* block = NULL;
	uint32_t pass, i, k, condTotal = 0;

	rtMemrefs = runtime->memrefs;
	modCount = 0;
	for (list = &rtMemrefs->modified_memrefs; list; list = list->next) {
		modCount += list->count;
	}
	for (i = 0; i < runtime->trigger_count; i++) {
		const rc_condset_t* s = NULL;
		if (usable(&runtime->triggers[i])) {
			while ((s = nextSet(runtime->triggers[i].trigger, s)) != NULL) {
				condTotal += condCount(s);
			}
		}
	}

	// Gate and memref indices are 16-bit (0xFFFF ends a hash chain)
	if (modCount > 0xFFFF || condTotal >= 0xFFFF) {
		return 0;
	}

	prog = NULL;
	for (pass = 0; pass < 2; pass++) {
		uint8_t* scratch;
		if (pass) {
			blockBytes = entryCount * sizeof(Entry) + gateCount * sizeof(Gate) + lazyCount * sizeof(Lazy) +
			             alwaysCount * sizeof(*always) + progLen * sizeof(*prog);
			block = (uint8_t*)malloc(blockBytes);
			if (!block) {
				return 0;
			}
			entries = (Entry*)block;
			gates = (Gate*)(entries + entryCount);
			lazies = (Lazy*)(gates + gateCount);
			always = (rc_modified_memref_t**)(lazies + lazyCount);
			prog = (uint16_t*)(always + alwaysCount);
		}
		scratch = (uint8_t*)malloc(modCount * (sizeof(*mods) + sizeof(*lazyIndex) + 1) +
		                           condTotal * (sizeof(*reps) + sizeof(*hashNext)));
		if (!scratch) {
			free(block);
			return 0;
		}
		mods = (rc_modified_memref_t**)scratch;
		reps = (rc_condition_t**)(mods + modCount);
		lazyIndex = (uint16_t*)(reps + condTotal);
		hashNext = lazyIndex + modCount;
		modFlags = (uint8_t*)(hashNext + condTotal);
		memset(modFlags, 0, modCount);
		memset(hashHead, 0xFF, sizeof(hashHead));

		k = 0;
		for (list = &rtMemrefs->modified_memrefs; list; list = list->next) {
			for (i = 0; i < list->count; i++) {
				mods[k++] = &list->items[i];
			}
		}

		// Which modified memrefs must be updated every frame: those read as
		// delta/prior, AddAddress reads (so all memory is read at the same
		// time as rcheevos does), those read before they are updated (not
		// expected), and everything they are computed from.
		for (i = 0; i < runtime->trigger_count; i++) {
			const rc_condset_t* s = NULL;
			if (!usable(&runtime->triggers[i])) {
				continue;
			}
			while ((s = nextSet(runtime->triggers[i].trigger, s)) != NULL) {
				rc_condition_t* c = rc_condset_get_conditions((rc_condset_t*)s);
				const uint32_t count = condCount(s);
				for (k = 0; k < count; k++) {
					flagHistory(&c[k].operand1);
					flagHistory(&c[k].operand2);
				}
			}
		}
		for (i = 0; i < modCount; i++) {
			uint8_t history;
			if (mods[i]->modifier_type == RC_OPERATOR_INDIRECT_READ ||
			    modRef(&mods[i]->parent, &history) >= (int32_t)i ||
			    modRef(&mods[i]->modifier, &history) >= (int32_t)i) {
				modFlags[i] |= ALWAYS;
			}
			flagHistory(&mods[i]->parent);
			flagHistory(&mods[i]->modifier);
		}
		for (;;) {
			int changed = 0;
			for (i = modCount; i-- > 0;) {
				if (modFlags[i] & ALWAYS) {
					uint8_t history;
					const int32_t a = modRef(&mods[i]->parent, &history);
					const int32_t b = modRef(&mods[i]->modifier, &history);
					if (a >= 0 && !(modFlags[a] & ALWAYS)) {
						modFlags[a] |= ALWAYS;
						changed = 1;
					}
					if (b >= 0 && !(modFlags[b] & ALWAYS)) {
						modFlags[b] |= ALWAYS;
						changed = 1;
					}
				}
			}
			if (!changed) {
				break;
			}
		}

		lazyCount = alwaysCount = 0;
		for (i = 0; i < modCount; i++) {
			if (modFlags[i] & ALWAYS) {
				if (pass) {
					always[alwaysCount] = mods[i];
				}
				alwaysCount++;
			} else {
				if (pass) {
					lazies[lazyCount].memref = mods[i];
					lazies[lazyCount].stamp = 0;
				}
				lazyIndex[i] = lazyCount++;
			}
		}

		entryCount = gateCount = skippableCount = 0;
		progLen = 0;
		for (i = 0; i < runtime->trigger_count; i++) {
			rc_trigger_t* t = runtime->triggers[i].trigger;
			const rc_condset_t* s = NULL;
			uint32_t slot, n = 0;
			int skippable, shortCircuit = 1;
			if (!usable(&runtime->triggers[i])) {
				continue;
			}
			skippable = t->requirement != NULL;
			while ((s = nextSet(t, s)) != NULL) {
				rc_condition_t* c = rc_condset_get_conditions((rc_condset_t*)s);
				const uint32_t count = condCount(s);
				const uint32_t other = otherStart(s);
				for (k = 0; k < count; k++) {
					const uint8_t type = c[k].type;
					if (c[k].required_hits || type == RC_CONDITION_PAUSE_IF ||
					    type == RC_CONDITION_MEASURED || type == RC_CONDITION_MEASURED_IF) {
						skippable = 0;
					}
					if (c[k].required_hits && k >= other && k < other + s->num_other_conditions &&
					    (type == RC_CONDITION_AND_NEXT || type == RC_CONDITION_OR_NEXT ||
					     type == RC_CONDITION_RESET_NEXT_IF)) {
						shortCircuit = 0;
					}
					mark(&c[k].operand1);
					mark(&c[k].operand2);
				}
			}

			if (pass) {
				entries[entryCount].trigger = t;
				entries[entryCount].id = runtime->triggers[i].id;
				entries[entryCount].prog = prog + progLen;
				entries[entryCount].shortCircuit = shortCircuit;
			}
			entryCount++;
			skippableCount += skippable;

			putGates(t, skippable, 0);
			slot = progLen;
			put(0);
			for (k = 0; k < modCount; k++) {
				if (modFlags[k] & MARK) {
					modFlags[k] &= ~MARK;
					put(lazyIndex[k]);
					n++;
				}
			}
			if (prog) {
				prog[slot] = n;
			}
			putGates(t, skippable, 1);
		}

		if (pass) {
			for (i = 0; i < gateCount; i++) {
				gates[i].cond = reps[i];
				gates[i].stamp = 0;
			}
		}
		free(scratch);
	}
	frame = 0;
	return 1;
}

void raFastGetInfo(struct RaFastInfo* info) {
	info->triggers = entryCount;
	info->skippable = skippableCount;
	info->gates = gateCount;
	info->lazy = lazyCount;
	info->always = alwaysCount;
	info->bytes = blockBytes;
}

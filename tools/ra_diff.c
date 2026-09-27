/*
	Differential test of the engine's fast path (ra_fast.c) against stock
	rcheevos: tools/ra_diff.sh builds this with -DRA_REF (stock) and without,
	runs both over the same RAM sequence and compares their output.

	  ra_diff <set.txt|synth> <seed> <frames> <random|gameplay|synth>

	One line per frame: frame number, achievements that triggered (sorted),
	a digest of everything that decides future unlocks, and a digest that
	also includes the exact trigger states (informational: rcheevos's
	PRIMED/PAUSED/ACTIVE distinction doesn't decide unlocks).

	Unlock digest, per achievement in set order:
	  - not active / WAITING / active (ACTIVE, PAUSED, PRIMED) / triggered
	  - measured value
	  - hit counts that can decide truth: conditions with a hit target, and
	    AddHits/SubHits feeding one.  (Counts of conditions without a target
	    never make a condition true.)
*/
#include "ra_host.h"

static uint32_t hash;
static void mix(uint32_t v) {
	for (int i = 0; i < 4; i++, v >>= 8) hash = (hash ^ (v & 0xFF)) * 16777619u;
}

static int isCombining(uint8_t type) {
	switch (type) {
		case RC_CONDITION_ADD_HITS: case RC_CONDITION_SUB_HITS: case RC_CONDITION_AND_NEXT:
		case RC_CONDITION_OR_NEXT: case RC_CONDITION_RESET_NEXT_IF: case RC_CONDITION_REMEMBER:
		case RC_CONDITION_ADD_SOURCE: case RC_CONDITION_SUB_SOURCE: case RC_CONDITION_ADD_ADDRESS:
			return 1;
	}
	return 0;
}

static void digestTrigger(const rc_trigger_t* t) {
	for (rc_condset_t* s = t->requirement ? t->requirement : t->alternative; s;
	     s = (s == t->requirement) ? t->alternative : s->next) {
		rc_condition_t* c = rc_condset_get_conditions(s);
		if (!c) continue;
		const int n = s->num_pause_conditions + s->num_reset_conditions + s->num_hittarget_conditions +
		              s->num_measured_conditions + s->num_other_conditions;
		for (int k = 0; k < n; k++) {
			int relevant;
			if (c[k].type == RC_CONDITION_ADD_HITS || c[k].type == RC_CONDITION_SUB_HITS) {
				int j = k + 1;
				while (j < n && isCombining(c[j].type)) j++;
				relevant = j < n && c[j].required_hits;
			} else {
				relevant = c[k].required_hits != 0;
			}
			if (relevant) mix(c[k].current_hits);
		}
	}
	mix(t->measured_value);
}

int main(int argc, char** argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: ra_diff <set.txt|synth> <seed> <frames> <random|gameplay|synth>\n");
		return 2;
	}
	const uint32_t seed = strtoul(argv[2], NULL, 10);
	const int frames = atoi(argv[3]);
	const char* mode = argv[4];
	seedRng(seed);
	if (strcmp(argv[1], "synth") == 0) synthSet(150);
	else loadSet(argv[1]);
	collectHints();
	engineInit();

	for (int f = 0; f < frames; f++) {
		if (strcmp(mode, "gameplay") == 0) stepGameplay(1);
		else if (strcmp(mode, "synth") == 0) stepSynth();
		else stepRandom();
		engineFrame();

		uint32_t sorted[MAX_ACH];
		memcpy(sorted, events, eventCount * sizeof(*events));
		for (int i = 1; i < eventCount; i++)
			for (int j = i; j > 0 && sorted[j - 1] > sorted[j]; j--) {
				uint32_t x = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = x;
			}

		uint32_t unlockHash, exactHash;
		hash = 2166136261u;
		for (int i = 0; i < achCount; i++) {
			const rc_trigger_t* t = rc_runtime_get_achievement(&rt, achs[i].id);
			int cls = 0;
			if (t) {
				cls = t->state == RC_TRIGGER_STATE_WAITING ? 1 : t->state == RC_TRIGGER_STATE_TRIGGERED ? 3 :
				      rc_trigger_state_active(t->state) ? 2 : 4;
			} else if (i < parsed) {
				cls = 3;  // RA_REF deactivates triggered achievements (or it didn't parse: same in both)
			}
			mix(cls);
			if (cls == 1 || cls == 2) digestTrigger(t);
		}
		unlockHash = hash;
		for (int i = 0; i < achCount; i++) {
			const rc_trigger_t* t = rc_runtime_get_achievement(&rt, achs[i].id);
			mix(t && t->state != RC_TRIGGER_STATE_TRIGGERED ? t->state : 0);
		}
		exactHash = hash;

		printf("%d", f);
		for (int i = 0; i < eventCount; i++) printf(" T%u", sorted[i]);
		printf(" %08x %08x\n", unlockHash, exactHash);
	}
	return 0;
}

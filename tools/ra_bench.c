/*
	Host benchmark: evaluate a DS achievement set (sd:/_nds/ra/sets format)
	the way the ARM7 engine does, against a fake 4MB main RAM, so valgrind
	can count instructions per frame (tools/ra_bench.sh).

	  ra_bench <set.txt> <frames> [zero|gameplay|<ramfile>]

	Built with -DRA_REF (stock rcheevos), -DRA_SC (always short-circuit) or
	neither (the engine now: ra_fast.c), see ra_host.h.  The set is first
	activated the way the engine does (a few achievements per frame) on the
	chosen RAM; then <frames> frames are run through benchFrame(), which is
	what callgrind counts (--toggle-collect=benchFrame).

	  zero      all RAM 0: every gate false
	  gameplay  Tetris DS Marathon being played (stepGameplay in ra_host.h)
	  ramfile   a 4MB dump, fixed
*/
#include "ra_host.h"

__attribute__((noinline)) void benchFrame(void) {
	engineFrame();
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: ra_bench <set.txt> <frames> [zero|gameplay|ramfile]\n");
		return 2;
	}
	const int frames = atoi(argv[2]);
	const char* mode = argc > 3 ? argv[3] : "zero";
	const int gameplay = strcmp(mode, "gameplay") == 0;
	if (!gameplay && strcmp(mode, "zero") != 0) {
		FILE* r = fopen(mode, "rb");
		if (!r || fread(ram, 1, sizeof(ram), r) == 0) {
			perror(mode);
			return 2;
		}
		fclose(r);
	}
	seedRng(1);
	loadSet(argv[1]);
	collectHints();
	engineInit();

	// Activation, as in the engine (and ra_fast.c prepared at the end)
	int warm = 0;
	while (parsed < achCount || warm < 2) {
		if (gameplay) stepGameplay(0);
		engineFrame();
		warm += parsed == achCount;
	}
	int unlocks = 0, conditions = 0, memrefs = 0;
	for (int i = 0; i < achCount; i++) {
		for (const char* p = achs[i].memaddr; *p; p++) conditions += (*p == '_');
		conditions++;
	}
	for (rc_memref_list_t* l = rt.memrefs ? &rt.memrefs->memrefs : NULL; l; l = l->next) memrefs += l->count;
	printf("%d achievements, ~%d conditions, %d memrefs, %d activation frames\n", achCount, conditions, memrefs, frameNo);
#if !defined(RA_REF) && !defined(RA_SC)
	struct RaFastInfo info;
	raFastGetInfo(&info);
	printf("fast path: %u triggers, %u skippable, %u gates, %u lazy / %u always modified memrefs, %u bytes\n",
	       info.triggers, info.skippable, info.gates, info.lazy, info.always, info.bytes);
#endif

	for (int i = 0; i < frames; i++) {
		if (gameplay) stepGameplay(0);
		benchFrame();
		unlocks += eventCount;
	}
	printf("%d frames (%s), %d unlocks\n", frames, mode, unlocks);
	return 0;
}

/*
	Host benchmark: evaluate a DS achievement set (sd:/_nds/ra/sets format)
	the way the ARM7 engine does, against a fake 4MB main RAM, so valgrind
	can count instructions per frame.

	gcc -O2 -Iexternal/rcheevos/include -Iexternal/rcheevos/src tools/ra_bench.c \
	    external/rcheevos/src/rcheevos/{alloc,condition,condset,format,lboard,memref,operand,richpresence,runtime,trigger,value}.c \
	    external/rcheevos/src/{rc_util,rc_compat}.c external/rcheevos/src/rhash/md5.c -lm -o ra_bench
	valgrind --tool=callgrind ./ra_bench set.txt 100
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rc_runtime.h"
#include "rcheevos/rc_internal.h"

static unsigned char ram[0x400000];
static unsigned long peeks;

static uint32_t peek(uint32_t address, uint32_t numBytes, void* ud) {
	(void)ud;
	peeks++;
	if (address + numBytes > sizeof(ram)) return 0;
	const unsigned char* p = ram + address;
	switch (numBytes) {
		case 1: return p[0];
		case 2: return p[0] | (p[1] << 8);
		case 4: return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
	}
	return 0;
}

static void onEvent(const rc_runtime_event_t* e) { (void)e; }

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: ra_bench set.txt frames [ramfile]\n");
		return 2;
	}
	FILE* f = fopen(argv[1], "rb");
	static char set[0x40000];
	size_t n = fread(set, 1, sizeof(set) - 1, f);
	fclose(f);
	set[n] = 0;
	if (argc > 3) {
		FILE* r = fopen(argv[3], "rb");
		fread(ram, 1, sizeof(ram), r);
		fclose(r);
	}

	rc_runtime_t rt;
	rc_runtime_init(&rt);
	int count = 0, conditions = 0;
	for (char* line = strtok(set, "\n"); line; line = strtok(NULL, "\n")) {
		if (strncmp(line, "ach\t", 4) != 0) continue;
		char* fields[6] = {0};
		int nf = 0;
		for (char* p = line; nf < 6; ) {
			fields[nf++] = p;
			p = strchr(p, '\t');
			if (!p) break;
			*p++ = 0;
		}
		if (rc_runtime_activate_achievement(&rt, strtoul(fields[1], NULL, 10), fields[3], NULL, 0) == RC_OK) {
			count++;
			for (char* p = fields[3]; *p; p++) conditions += (*p == '_');
			conditions++;
		}
	}
	int memrefs = 0;
	for (rc_memref_list_t* l = rt.memrefs ? &rt.memrefs->memrefs : NULL; l; l = l->next) memrefs += l->count;
	printf("%d achievements, ~%d conditions, %d memrefs\n", count, conditions, memrefs);

	const int frames = atoi(argv[2]);
	for (int i = 0; i < frames; i++) {
		rc_runtime_do_frame(&rt, onEvent, peek, NULL, NULL);
	}
	printf("%d frames, %lu peeks (%lu per frame)\n", frames, peeks, peeks / (frames ? frames : 1));
	return 0;
}

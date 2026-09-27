#!/bin/sh
# Count instructions per frame for a set, in a Debian container (gcc +
# valgrind), from the repo root:
#
#   ./tools/ra_bench.sh [set.txt] [zero|gameplay|ramfile]
#   PROFILE=1 ./tools/ra_bench.sh ...   # also the top functions of each
#
# Three builds of tools/ra_bench.c: stock rcheevos (the engine's old loop),
# trigger.c always short-circuiting (the engine before ra_fast.c) and the
# engine now (ra_fast.c, trigger.c patched as in the engine Makefile).
set -e
SET=${1:-tools/tetris_set.txt}
MODE=${2:-zero}
docker run --rm -e PROFILE="${PROFILE:-}" -e SET="$SET" -e MODE="$MODE" -e FRAMES="${FRAMES:-300}" \
	-v "$(pwd)":/src -w /src debian:bookworm-slim sh -ec '
apt-get update -qq >/dev/null && apt-get install -y -qq gcc valgrind libc6-dev >/dev/null 2>&1
R=external/rcheevos/src
E=retail/cardenginei/arm7_ra/source
sed "s/memset(&eval_state, 0, sizeof(eval_state));/& eval_state.can_short_curcuit = 1;/" \
	$R/rcheevos/trigger.c > /tmp/trigger_always.c
sed -e "s/^#include \"rc_internal.h\"/&\nunsigned char ra_short_circuit;/" \
	-e "s/memset(&eval_state, 0, sizeof(eval_state));/& eval_state.can_short_curcuit = ra_short_circuit;/" \
	$R/rcheevos/trigger.c > /tmp/trigger_sc.c
COMMON="$R/rcheevos/alloc.c $R/rcheevos/condition.c $R/rcheevos/condset.c $R/rcheevos/format.c
	$R/rcheevos/lboard.c $R/rcheevos/memref.c $R/rcheevos/operand.c $R/rcheevos/richpresence.c
	$R/rcheevos/runtime.c $R/rcheevos/value.c $R/rc_util.c $R/rc_compat.c $R/rhash/md5.c"
FLAGS="-O2 -g -DNDEBUG -I$R/../include -I$R -I$R/rcheevos -I$E"
gcc $FLAGS -DRA_REF tools/ra_bench.c $COMMON $R/rcheevos/trigger.c -lm -o /tmp/stock
gcc $FLAGS -DRA_SC tools/ra_bench.c $COMMON /tmp/trigger_always.c -lm -o /tmp/always_sc
gcc $FLAGS tools/ra_bench.c $E/ra_fast.c $COMMON /tmp/trigger_sc.c -lm -o /tmp/fast
for b in stock always_sc fast; do
	valgrind --tool=callgrind --toggle-collect=benchFrame --callgrind-out-file=/tmp/cg.$b \
		/tmp/$b $SET $FRAMES $MODE > /tmp/out.$b 2>/dev/null
	total=$(callgrind_annotate /tmp/cg.$b | sed -n "s/^ *\([0-9,]*\) .*PROGRAM TOTALS.*/\1/p" | tr -d ,)
	echo "== $b"
	cat /tmp/out.$b
	echo "instructions per frame: $((total / FRAMES))"
	if [ -n "$PROFILE" ]; then
		callgrind_annotate --inclusive=no /tmp/cg.$b | grep -E "^ *[0-9,]+ " | sed -n "2,13p"
	fi
done
'

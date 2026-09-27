#!/bin/sh
# Count instructions per frame for a set, in a Debian container (gcc +
# valgrind), from the repo root:
#
#   ./tools/ra_bench.sh [set.txt] [ramfile]
#   SC=1 ./tools/ra_bench.sh ...     # with the engine's short-circuit patch
set -e
SET=${1:-tools/tetris_set.txt}
RAM=${2:-}
docker run --rm -e PROFILE="${PROFILE:-}" -e SC="${SC:-}" -e SET="$SET" -e RAM="$RAM" -v "$(pwd)":/src -w /src \
	debian:bookworm-slim sh -ec '
apt-get update -qq >/dev/null && apt-get install -y -qq gcc valgrind libc6-dev >/dev/null 2>&1
R=external/rcheevos/src
TRIGGER=$R/rcheevos/trigger.c
if [ -n "$SC" ]; then
	# same patch as the ARM7 engine build: stop at the first false condition
	sed "s/memset(&eval_state, 0, sizeof(eval_state));/&\n  eval_state.can_short_curcuit = 1;/" \
		$TRIGGER > /tmp/trigger.c
	TRIGGER=/tmp/trigger.c
fi
gcc -O2 -g -DNDEBUG -I$R/../include -I$R -I$R/rcheevos tools/ra_bench.c \
	$R/rcheevos/alloc.c $R/rcheevos/condition.c $R/rcheevos/condset.c $R/rcheevos/format.c \
	$R/rcheevos/lboard.c $R/rcheevos/memref.c $R/rcheevos/operand.c $R/rcheevos/richpresence.c \
	$R/rcheevos/runtime.c $TRIGGER $R/rcheevos/value.c \
	$R/rc_util.c $R/rc_compat.c $R/rhash/md5.c -lm -o /tmp/ra_bench
base=$(valgrind --tool=callgrind --callgrind-out-file=/tmp/cg0 /tmp/ra_bench $SET 0 $RAM 2>&1 | sed -n "s/.*I *refs: *//p" | tr -d ,)
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg /tmp/ra_bench $SET 100 $RAM > /tmp/out 2>&1
cat /tmp/out | grep -E "achievements|frames"
total=$(sed -n "s/.*I *refs: *//p" /tmp/out | tr -d ,)
echo "instructions per frame: $(( (total - base) / 100 ))"
if [ -n "$PROFILE" ]; then
	valgrind --tool=callgrind --toggle-collect=rc_runtime_do_frame --callgrind-out-file=/tmp/cgf 		/tmp/ra_bench $SET 100 $RAM >/dev/null 2>&1
	callgrind_annotate --inclusive=no /tmp/cgf | grep -E "^ *[0-9,]+ " | head -20
fi
'

#!/bin/sh
# Differential test: the engine's fast path (ra_fast.c + patched trigger.c)
# against stock rcheevos (rc_runtime_do_frame, unpatched), over many seeds.
# From the repo root:
#
#   ./tools/ra_diff.sh [set.txt]         # default runs
#   SEEDS=200 FRAMES=20000 ./tools/ra_diff.sh
#
# Runs in a Debian container (gcc).  Fails on the first seed whose triggered
# achievements or unlock digest differ in any frame.  Without a set only the
# synthetic sets are run.
set -e
if [ "$1" != "--inside" ]; then
	# seccomp=unconfined: ASan needs ASLR off (setarch -R) on kernels with
	# vm.mmap_rnd_bits = 32, or it loops on DEADLYSIGNAL
	exec docker run --rm --security-opt seccomp=unconfined -e SEEDS="${SEEDS:-}" -e FRAMES="${FRAMES:-}" \
		-v "$(pwd)":/src -w /src debian:bookworm-slim sh tools/ra_diff.sh --inside "$1"
fi
SET=$2
apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev util-linux >/dev/null 2>&1
R=external/rcheevos/src
E=retail/cardenginei/arm7_ra/source
# same patch as the engine Makefile
sed -e "s/^#include \"rc_internal.h\"/&\nunsigned char ra_short_circuit;/" \
	-e "s/memset(&eval_state, 0, sizeof(eval_state));/& eval_state.can_short_curcuit = ra_short_circuit;/" \
	$R/rcheevos/trigger.c > /tmp/trigger_sc.c
grep -q "can_short_curcuit = ra_short_circuit" /tmp/trigger_sc.c
COMMON="$R/rcheevos/alloc.c $R/rcheevos/condition.c $R/rcheevos/condset.c $R/rcheevos/format.c
	$R/rcheevos/lboard.c $R/rcheevos/memref.c $R/rcheevos/operand.c $R/rcheevos/richpresence.c
	$R/rcheevos/runtime.c $R/rcheevos/value.c $R/rc_util.c $R/rc_compat.c $R/rhash/md5.c"
if setarch -R true 2>/dev/null; then
	RUN="setarch -R"
	export ASAN_OPTIONS=detect_leaks=0  # the harness never frees its runtime
	SAN="-fsanitize=address,undefined"
else
	echo "note: can't turn ASLR off, running without AddressSanitizer"
	RUN=
	SAN="-fsanitize=undefined"
fi
# rcheevos (value.c) adds typed values with signed overflow: not checked
FLAGS="-O1 -g $SAN -fno-sanitize=signed-integer-overflow -fno-sanitize-recover=undefined -DNDEBUG -I$R/../include -I$R -I$R/rcheevos -I$E"
gcc $FLAGS -DRA_REF tools/ra_diff.c $COMMON $R/rcheevos/trigger.c -lm -o /tmp/ref
gcc $FLAGS tools/ra_diff.c $E/ra_fast.c $COMMON /tmp/trigger_sc.c -lm -o /tmp/fast

SEEDS=${SEEDS:-40}
FRAMES=${FRAMES:-10000}
fail=0
run() { # set mode seeds frames
	events=0; exact=0; seed=1
	while [ $seed -le $3 ]; do
		if ! timeout 600 $RUN /tmp/ref "$1" $seed $4 $2 > /tmp/a 2>/tmp/a.err ||
		   ! timeout 600 $RUN /tmp/fast "$1" $seed $4 $2 > /tmp/b 2>/tmp/b.err; then
			echo "FAILED TO RUN set=$1 mode=$2 seed=$seed"
			head -20 /tmp/a.err /tmp/b.err
			fail=1
			return
		fi
		awk '{$NF=""; print}' /tmp/a > /tmp/a.u
		awk '{$NF=""; print}' /tmp/b > /tmp/b.u
		if ! cmp -s /tmp/a.u /tmp/b.u; then
			echo "MISMATCH set=$1 mode=$2 seed=$seed"
			diff /tmp/a.u /tmp/b.u | head -5
			fail=1
			return
		fi
		cmp -s /tmp/a /tmp/b || exact=$((exact + 1))
		events=$((events + $(grep -o " T[0-9]*" /tmp/a | wc -l)))
		seed=$((seed + 1))
	done
	echo "ok  $1 $2: $3 seeds x $4 frames, $events unlocks, identical (exact-state digest differed in $exact seeds)"
}
if [ -n "$SET" ]; then
	run "$SET" random $SEEDS $FRAMES
	run "$SET" gameplay $((SEEDS / 4 + 1)) $FRAMES
fi
run synth synth $((SEEDS * 5)) $((FRAMES / 5))
exit $fail

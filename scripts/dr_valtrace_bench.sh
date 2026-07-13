#!/usr/bin/env bash
# dr_valtrace_bench.sh — data-flow DR value-capture microbenchmark
# (examples/dr_valtrace_bench.c) run under the clean-call and inlined value
# clients across fresh processes. The "direction-of-travel" cost check for
# taint-tier Increment 3: the inlined drmgr/drreg/drx_buf client must show a
# measurable per-instruction capture-cost drop vs the per-instruction clean call.
# Invoked by mk/native-trace.mk's dr-valtrace-bench.
#
# Primary metric is the ISOLATED capture window (ASMTEST_DRVAL_BENCH makes the
# producer emit DRVAL_CAPTURE_NS = the instrumented run + drx_buf flush only,
# excluding the symmetric DR init and app-side replay). We take the min over
# SAMPLES runs (least noise). The clean-call vs inlined DELTA is the pure
# per-instruction capture-cost difference — DR base overhead and the routine's
# own execution are identical for both clients.
#
# Env (set by the make lane): ITERS SAMPLES CLEAN INLINED DRLIB BIN
set -euo pipefail

: "${ITERS:=40000}"
: "${SAMPLES:=5}"

# run_min <client.so> -> echoes "<min_capture_ns> <steps>", or "SKIP".
run_min() {
  local client="$1" min="" steps="" err out cap_ns
  err=$(mktemp)
  for _ in $(seq 1 "$SAMPLES"); do
    out=$(ASMTEST_DRVAL_CLIENT="$client" ASMTEST_DR_LIB="$DRLIB" \
          ASMTEST_DRVAL_BENCH=1 "$BIN" "$ITERS" 2>"$err")
    steps=$(printf '%s\n' "$out" | awk 'NR==1{print $2}')
    if [ "${steps:-0}" = "0" ]; then rm -f "$err"; echo "SKIP"; return 0; fi
    cap_ns=$(awk '/^DRVAL_CAPTURE_NS/{print $2; exit}' "$err")
    if [ -z "${cap_ns:-}" ]; then rm -f "$err"; echo "NOCAP"; return 0; fi
    if [ -z "$min" ] || [ "$cap_ns" -lt "$min" ]; then min="$cap_ns"; fi
  done
  rm -f "$err"
  echo "$min $steps"
}

clean_res=$(run_min "$CLEAN")
inlined_res=$(run_min "$INLINED")

case "$clean_res $inlined_res" in
  *SKIP*) echo "# SKIP dr-valtrace-bench: a value client self-skipped (DR unavailable)"; echo "1..0 # skipped"; exit 0 ;;
  *NOCAP*) echo "# dr-valtrace-bench: producer did not emit DRVAL_CAPTURE_NS (build too old?)"; echo "1..0 # skipped"; exit 0 ;;
esac

clean_ns=$(echo "$clean_res"   | awk '{print $1}')
steps=$(echo "$clean_res"      | awk '{print $2}')
inlined_ns=$(echo "$inlined_res" | awk '{print $1}')

awk -v c="$clean_ns" -v i="$inlined_ns" -v s="$steps" -v it="$ITERS" -v n="$SAMPLES" '
BEGIN {
  cps = c / s; ips = i / s; delta = cps - ips;
  printf "  iters=%d  dynamic in-region steps=%d  (min capture window of %d runs each)\n", it, s, n;
  printf "  clean-call capture : %9.3f ms  %8.1f ns/insn\n", c/1e6, cps;
  printf "  inlined    capture : %9.3f ms  %8.1f ns/insn\n", i/1e6, ips;
  printf "  per-insn capture drop: %.1f ns/insn  (%.2fx faster; %.1f%% less)\n", \
         delta, (ips>0 ? cps/ips : 0), (cps>0 ? 100.0*delta/cps : 0);
  if (i < c) { print "ok 1 - inlined per-instruction capture is measurably cheaper than the clean call"; print "1..1"; exit 0; }
  else { print "not ok 1 - inlined capture was NOT cheaper (clean=" c "ns inlined=" i "ns)"; print "1..1"; exit 1; }
}'

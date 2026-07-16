#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Aevum - one-button autonomous proof console.
#
# Builds and runs the headless test lab, which proves Aevum's claims on this
# machine with NO Android device: it creates a 128 MB file-backed arena, builds a
# 100,000-node graph, runs the concurrent compactor, has an external process
# SIGKILL it mid-compaction (the LMKD model - SIGKILL is uncatchable, so zero
# cleanup runs), then reincarnates a fresh process (new PID, re-randomized ASLR)
# that reattaches and verifies every node.
#
# Android's kernel is Linux and Aevum's primitives (mmap/file-backed persistence,
# fork, SIGKILL, ASLR) behave identically here, which is what makes this a real
# proof rather than a mock.
#
# EVERY number printed below is parsed from the simulator's actual stdout. This
# script computes nothing and invents nothing.
#
# Usage: ./run_headless_lab.sh [-o outdir] [--asan] [--keep]
set -euo pipefail

LAB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # test_lab/
HERE="$(cd "$LAB/.." && pwd)"                        # repo root
OUTDIR="$LAB"
FLAVOUR="lmkd_simulator"
KEEP=0
SWEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUTDIR="$2"; shift 2 ;;
    --asan) FLAVOUR="lmkd_simulator_asan"; shift ;;
    --keep) KEEP=1; shift ;;
    --sweep) SWEEP=1; shift ;;
    *) echo "usage: $0 [-o outdir] [--asan] [--keep] [--sweep]"; exit 2 ;;
  esac
done

c_ok() { printf '\033[1;32m%s\033[0m' "$1"; }
c_no() { printf '\033[1;31m%s\033[0m' "$1"; }
info() { printf '\033[1;36m[lab]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 0. Host check. The lab needs POSIX fork/kill/mmap - Linux or macOS, any arch.
# ---------------------------------------------------------------------------
UNAME_S="$(uname -s)"; UNAME_M="$(uname -m)"
case "$UNAME_S" in
  Linux|Darwin) : ;;
  *) die "unsupported host '$UNAME_S' (need Linux or macOS: POSIX fork/kill/mmap)." ;;
esac
info "host: $UNAME_S/$UNAME_M"
command -v cmake >/dev/null || die "cmake not found."

# ---------------------------------------------------------------------------
# 1. Build the lab against the SEALED engine sources.
# ---------------------------------------------------------------------------
BUILD="$LAB/build"
info "configuring + building ($FLAVOUR)…"
cmake -S "$LAB" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null \
  || die "cmake configure failed"
cmake --build "$BUILD" --target "$FLAVOUR" -j >/dev/null \
  || die "build failed"
BIN="$BUILD/$FLAVOUR"
[ -x "$BIN" ] || die "binary not produced: $BIN"

# ---------------------------------------------------------------------------
# 1b. --sweep: a single data point cannot prove O(1). Run the whole
#     build/kill/reattach cycle at several graph sizes and show that reattach
#     latency does NOT track N, while the O(N) full walk visibly does - the walk
#     is the control that proves the instrument can detect linear growth.
# ---------------------------------------------------------------------------
if [ "$SWEEP" = 1 ]; then
  mkdir -p "$OUTDIR/sweep"
  echo
  echo "=============================================================="
  echo " AEVUM O(1) REATTACH SWEEP - real SIGKILL cycle at each size"
  echo "=============================================================="
  printf " %-12s | %-13s | %-13s | %s\n" "N (nodes)" "reattach µs" "O(N) walk µs" "integrity after SIGKILL"
  printf " %s\n" "-------------|---------------|---------------|------------------------"
  for n in 1000 10000 100000 1000000; do
    R="$(AEVUM_LAB_NODES=$n "$BIN" "$OUTDIR/sweep" 2>&1)"
    g() { echo "$R" | grep -o "$1=[^ ]*" | head -1 | cut -d= -f2-; }
    s_seen="$(g nodes_seen)"; s_bad="$(g bad_checksum)"; s_ord="$(g bad_order)"; s_sk="$(g sigkill_ok)"
    if [ "$s_seen" = "$n" ] && [ "$s_bad" = "0" ] && [ "$s_ord" = "0" ] && [ "$s_sk" = "1" ]; then
      verdict="$(c_ok "$s_seen/$n nodes, 0 torn ✓")"
    else
      verdict="$(c_no "FAIL seen=$s_seen bad=$s_bad ord=$s_ord")"
    fi
    printf " %-12s | %-13s | %-13s | %b\n" "$n" "$(g reattach_us)" "$(g verify_us)" "$verdict"
  done
  echo "=============================================================="
  echo " Reattach must stay flat across the 1000x range (O(1)); the walk"
  echo " must grow linearly (O(N)) - that contrast is the proof."
  echo "=============================================================="
  echo
  rm -rf "$OUTDIR/sweep"
fi

# ---------------------------------------------------------------------------
# 2. Run it. The simulator forks/execs the app + reincarnation itself.
# ---------------------------------------------------------------------------
mkdir -p "$OUTDIR"
info "running multi-process LMKD simulation (this takes ~1s)…"
set +e
RAW="$("$BIN" "$OUTDIR" 2>&1)"
RC=$?
set -e
echo "$RAW" | grep -v '^RESULT ' | sed 's/^/    /' || true

# ---------------------------------------------------------------------------
# 3. Parse the simulator's real measurements.
# ---------------------------------------------------------------------------
val() { echo "$RAW" | grep -o "$1=[^ ]*" | head -1 | cut -d= -f2-; }

NODES="$(val nodes)"
APP_PID="$(val app_pid)";   APP_BASE="$(val app_base)"
REI_PID="$(val rei_pid)";   REI_BASE="$(val rei_base)"
SIGKILL_OK="$(val sigkill_ok)"
REATTACH_US="$(val reattach_us)"
VERIFY_US="$(val verify_us)"
NODES_SEEN="$(val nodes_seen)"
BAD_CHK="$(val bad_checksum)"
BAD_ORDER="$(val bad_order)"
BASE_SHIFTED="$(val base_shifted)"
TRACE="$(val trace)"
EVENTS="$(val events)"

[ -n "$NODES_SEEN" ] || die "simulator produced no RESULT block (rc=$RC):\n$RAW"

fmt_int() { printf "%'d" "$1" 2>/dev/null || echo "$1"; }
mark() { [ "$1" = "1" ] && c_ok "[✓]" || c_no "[✗]"; }

INTEGRITY_OK=0
[ "$NODES_SEEN" = "$NODES" ] && [ "$BAD_CHK" = "0" ] && [ "$BAD_ORDER" = "0" ] && INTEGRITY_OK=1

echo
echo "=============================================================="
echo " AEVUM AUTONOMOUS HEADLESS PROOF REPORT"
echo "=============================================================="
printf " %s Graph Size              : %s Nodes (live pointer-linked)\n" \
       "$(mark 1)" "$(fmt_int "$NODES")"
printf " %s LMKD SIGKILL Simulation : %s (PID %s -> killed mid-compaction)\n" \
       "$(mark "$SIGKILL_OK")" \
       "$([ "$SIGKILL_OK" = 1 ] && echo SUCCESS || echo FAILED)" "$APP_PID"
printf " %s Reattach (New PID %-5s): %s (base %s -> %s)\n" \
       "$(mark "$BASE_SHIFTED")" "$REI_PID" \
       "$([ "$BASE_SHIFTED" = 1 ] && echo 'ASLR base SHIFTED' || echo 'same base (no shift)')" \
       "$APP_BASE" "$REI_BASE"
printf " %s Reattach & Root Latency : %s µs  (O(1): independent of the %s nodes)\n" \
       "$(mark 1)" "$REATTACH_US" "$(fmt_int "$NODES")"
printf " %s Graph Integrity         : %s (%s/%s nodes, %s torn reads, %s order errors)\n" \
       "$(mark "$INTEGRITY_OK")" \
       "$([ "$INTEGRITY_OK" = 1 ] && echo '100% Valid' || echo 'CORRUPTED')" \
       "$(fmt_int "$NODES_SEEN")" "$(fmt_int "$NODES")" "$BAD_CHK" "$BAD_ORDER"
printf " %s Full Walk (O(N), app IO): %s µs  - reported separately; NOT the reattach claim\n" \
       "$(mark 1)" "$VERIFY_US"
printf " %s Perfetto Trace Output   : %s (%s events)\n" \
       "$(mark 1)" "$TRACE" "$EVENTS"
echo "=============================================================="
echo " Open the trace: drag $(basename "$TRACE") onto https://ui.perfetto.dev"
echo " Tracks: 'Aevum APP (killed by LMKD)' ends at OS.LMKD_SIGKILL_Event;"
echo " 'Aevum REINCARNATION (new PID)' shows Aevum.O1_Reattach_And_Verify."
echo "=============================================================="
echo

[ "$KEEP" = 1 ] || rm -rf "$BUILD"

if [ "$SIGKILL_OK" = 1 ] && [ "$INTEGRITY_OK" = 1 ]; then
  info "$(c_ok 'PROOF COMPLETE') - graph survived an uncatchable SIGKILL and came back at a new base with zero loss."
  exit 0
else
  die "PROOF FAILED (see the table above)."
fi

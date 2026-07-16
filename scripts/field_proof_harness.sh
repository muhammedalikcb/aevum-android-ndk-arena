#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Aevum field-proof harness - one command turns an ARM64 Android device into a
# physical proof of the whitepaper's claims (docs/WHITEPAPER.md §5):
#
#   C1  ART GC does zero work on arena memory        (Perfetto + Debug.getRuntimeStat)
#   C2  reattach is O(1) in graph size               (LMKD kill → relaunch timing)
#   C3  compaction never pauses mutators             (Perfetto: Aevum.* vs ART GC slices)
#
# It locks CPU frequencies for stable numbers, installs and drives the app,
# captures a Perfetto trace around a real LMKD-equivalent kill, and hands the
# trace to scripts/parse_perfetto.py. Every measurement comes from the device;
# this script fabricates nothing.
#
# The app must emit these logcat lines (the benchmark/showcase already compute
# the values - see benchmark/AevumBenchmark.kt and showcase/…/ShowcaseApp.kt):
#   AEVUM_PROOF reattach_us=<u> java_deserialize_us=<u> nodes=<n>
#   AEVUM_PROOF gc_arena  gc_count=<c> gc_pause_ms=<m> blocking_gc_ms=<b>
#   AEVUM_PROOF gc_heap   gc_count=<c> gc_pause_ms=<m> blocking_gc_ms=<b>
#
# Usage:  ./field_proof_harness.sh [-s <serial>] [-p <package>] [-o <outdir>]
set -euo pipefail

PKG="dev.aevum.showcase"
ACT="dev.aevum.showcase/.ShowcaseActivity"
SERIAL=""
OUTDIR="aevum-field-proof-$(date +%Y%m%d-%H%M%S)"
TRACE_SECS=15

while getopts "s:p:o:t:" opt; do
  case "$opt" in
    s) SERIAL="$OPTARG" ;;
    p) PKG="$OPTARG" ;;
    o) OUTDIR="$OPTARG" ;;
    t) TRACE_SECS="$OPTARG" ;;
    *) echo "usage: $0 [-s serial] [-p package] [-o outdir] [-t trace_secs]"; exit 2 ;;
  esac
done

ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

log()  { printf '\033[1;36m[harness]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[fail]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 0. Preconditions
# ---------------------------------------------------------------------------
command -v adb >/dev/null || die "adb not on PATH (install platform-tools)."
"${ADB[@]}" get-state >/dev/null 2>&1 || die "no device (adb get-state failed)."
ABI="$("${ADB[@]}" shell getprop ro.product.cpu.abi | tr -d '\r')"
SDK="$("${ADB[@]}" shell getprop ro.build.version.sdk | tr -d '\r')"
MODEL="$("${ADB[@]}" shell getprop ro.product.model | tr -d '\r')"
log "device: $MODEL  abi=$ABI  sdk=$SDK"
case "$ABI" in arm64-v8a|armeabi-v7a) : ;; *) die "Aevum targets ARM; device abi=$ABI." ;; esac
mkdir -p "$OUTDIR"

HAVE_ROOT=0
if "${ADB[@]}" root >/dev/null 2>&1 && "${ADB[@]}" shell 'id -u' 2>/dev/null | grep -q '^0'; then
  HAVE_ROOT=1
fi

# ---------------------------------------------------------------------------
# 1. Lock CPU frequency (stable measurements). Requires root/userdebug; the
#    harness proceeds either way and records whether frequencies were pinned.
# ---------------------------------------------------------------------------
FREQ_LOCKED="no"
if [ "$HAVE_ROOT" = 1 ]; then
  log "locking CPU governors to 'performance' and pinning max frequency"
  "${ADB[@]}" shell '
    for c in /sys/devices/system/cpu/cpu[0-9]*; do
      [ -w "$c/cpufreq/scaling_governor" ] || continue
      echo performance > "$c/cpufreq/scaling_governor" 2>/dev/null
      max=$(cat "$c/cpufreq/cpuinfo_max_freq" 2>/dev/null) || continue
      echo "$max" > "$c/cpufreq/scaling_min_freq" 2>/dev/null
      echo "$max" > "$c/cpufreq/scaling_max_freq" 2>/dev/null
    done
    # disable core hotplug bounce where present
    for f in /sys/devices/system/cpu/cpu[1-9]*/online; do echo 1 > "$f" 2>/dev/null || true; done
  ' && FREQ_LOCKED="yes" || warn "could not fully lock frequencies"
else
  warn "no adb root: CPU frequencies NOT pinned (numbers will be noisier)."
fi
echo "device=$MODEL abi=$ABI sdk=$SDK freq_locked=$FREQ_LOCKED" > "$OUTDIR/env.txt"

# ---------------------------------------------------------------------------
# 2. Install. Prefer a locally-built APK; else assume it is already installed.
# ---------------------------------------------------------------------------
APK="${AEVUM_APK:-}"
if [ -z "$APK" ]; then
  APK="$(ls ../showcase/build/outputs/apk/*/*.apk 2>/dev/null | head -1 || true)"
fi
if [ -n "$APK" ] && [ -f "$APK" ]; then
  log "installing $APK"
  "${ADB[@]}" install -r -g "$APK" >/dev/null || die "install failed"
elif "${ADB[@]}" shell pm path "$PKG" >/dev/null 2>&1; then
  log "using already-installed $PKG"
else
  die "no APK (set AEVUM_APK=... or ./gradlew :showcase:assembleRelease) and $PKG not installed."
fi

"${ADB[@]}" logcat -c || true

# ---------------------------------------------------------------------------
# 3. Perfetto capture around a compaction-heavy run.
#    Config includes the app's atrace tag so Aevum.* sections are recorded.
# ---------------------------------------------------------------------------
log "starting Perfetto capture (${TRACE_SECS}s), app atrace enabled for $PKG"
PERF_CFG=$(cat <<EOF
buffers { size_kb: 63488 fill_policy: RING_BUFFER }
duration_ms: $((TRACE_SECS * 1000))
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      atrace_apps: "$PKG"
      atrace_categories: "gfx" atrace_categories: "view" atrace_categories: "sched"
      atrace_categories: "freq" atrace_categories: "am"  atrace_categories: "dalvik"
      ftrace_events: "sched/sched_switch"
    }
  }
}
data_sources { config { name: "linux.process_stats" } }
EOF
)
"${ADB[@]}" shell "cat > /data/misc/perfetto-configs/aevum.cfg" <<<"$PERF_CFG" 2>/dev/null || \
  { "${ADB[@]}" shell "mkdir -p /data/local/tmp && cat > /data/local/tmp/aevum.cfg" <<<"$PERF_CFG"; PCFG=/data/local/tmp/aevum.cfg; }
PCFG="${PCFG:-/data/misc/perfetto-configs/aevum.cfg}"

"${ADB[@]}" shell "rm -f /data/local/tmp/aevum.perfetto-trace"
"${ADB[@]}" shell "perfetto --txt -c $PCFG -o /data/local/tmp/aevum.perfetto-trace" &
PERF_PID=$!

# Drive the app: launch, generate/compact for a while (the app streams tokens and
# runs compact() on a background thread).
log "launching $ACT and driving a compaction-heavy workload"
"${ADB[@]}" shell am start -n "$ACT" >/dev/null
sleep 2
# Poke the UI to stream tokens + trigger compactions (monkey gives deterministic taps).
"${ADB[@]}" shell monkey -p "$PKG" --pct-syskeys 0 --throttle 40 -v 400 >/dev/null 2>&1 || true
sleep 3

# ---------------------------------------------------------------------------
# 4. LMKD-equivalent process death, then relaunch and time the reattach.
# ---------------------------------------------------------------------------
log "backgrounding then killing $PKG (LMKD-equivalent: am kill)"
"${ADB[@]}" shell input keyevent KEYCODE_HOME
sleep 1
"${ADB[@]}" shell am kill "$PKG" || "${ADB[@]}" shell am force-stop "$PKG"
sleep 1
KILL_T=$("${ADB[@]}" shell 'date +%s%3N' | tr -d '\r')
log "relaunching - app must reattach the Aevum arena in onCreate"
"${ADB[@]}" shell am start -n "$ACT" >/dev/null
sleep 3

wait "$PERF_PID" 2>/dev/null || true
log "pulling Perfetto trace"
"${ADB[@]}" pull /data/local/tmp/aevum.perfetto-trace "$OUTDIR/aevum.perfetto-trace" >/dev/null || \
  warn "could not pull trace"

# ---------------------------------------------------------------------------
# 5. Collect the app's own measurements from logcat.
# ---------------------------------------------------------------------------
"${ADB[@]}" logcat -d | grep "AEVUM_PROOF" > "$OUTDIR/proof.log" || true
if [ -s "$OUTDIR/proof.log" ]; then
  log "device-reported measurements:"
  sed 's/^/    /' "$OUTDIR/proof.log"
else
  warn "no AEVUM_PROOF logcat lines - ensure the app logs them (see header)."
fi

# ---------------------------------------------------------------------------
# 6. Analyze the trace: Aevum phases vs ART GC (proves C1/C3).
# ---------------------------------------------------------------------------
if [ -f "$OUTDIR/aevum.perfetto-trace" ] && command -v python3 >/dev/null; then
  log "analyzing trace with parse_perfetto.py"
  python3 "$(dirname "$0")/parse_perfetto.py" \
      --trace "$OUTDIR/aevum.perfetto-trace" \
      --out-md "$OUTDIR/report.md" --out-csv "$OUTDIR/slices.csv" \
      --package "$PKG" || warn "trace analysis failed (is the perfetto py package installed?)"
fi

log "restoring CPU governors to schedutil"
[ "$HAVE_ROOT" = 1 ] && "${ADB[@]}" shell '
  for c in /sys/devices/system/cpu/cpu[0-9]*; do
    echo schedutil > "$c/cpufreq/scaling_governor" 2>/dev/null || true
  done' || true

log "done. artifacts in: $OUTDIR/"
ls -1 "$OUTDIR" | sed 's/^/    /'

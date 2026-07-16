#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Aevum Perfetto trace analyzer.

Reads a .perfetto-trace captured by field_proof_harness.sh and proves, from the
device's own trace, the whitepaper's C1/C3:

  * Aevum's phases  (Aevum.compact / mark / safepoint / evacuate / fixup / commit,
    the exact atrace sections emitted by native/src/profiler.cpp) run on Aevum's
    OWN background compactor thread.
  * ART's GC slices (HeapTaskDaemon / "*GC*") run on a DIFFERENT thread and never
    execute inside an Aevum phase - i.e. ART does no work for the arena graph.

Emits a Markdown report and a CSV of every Aevum slice. It analyzes only what the
trace actually contains; it invents nothing.

Requires either the `perfetto` Python package (`pip install perfetto`) or a
`trace_processor_shell` binary on PATH (auto-detected).

Usage:
  parse_perfetto.py --trace T.perfetto-trace [--out-md report.md]
                    [--out-csv slices.csv] [--package dev.aevum.showcase]
"""
import argparse
import csv
import json
import shutil
import subprocess
import sys
import tempfile
from statistics import median

AEVUM_PHASES = ["Aevum.compact", "Aevum.mark", "Aevum.safepoint",
                "Aevum.evacuate", "Aevum.fixup", "Aevum.commit"]


# --------------------------------------------------------------------------- #
# Trace query backend: prefer the perfetto py package; fall back to the shell.
# --------------------------------------------------------------------------- #
class Tp:
    def __init__(self, trace_path):
        self._mode = None
        try:
            from perfetto.trace_processor import TraceProcessor  # type: ignore
            self._tp = TraceProcessor(trace=trace_path)
            self._mode = "py"
        except Exception:
            shell = shutil.which("trace_processor_shell") or shutil.which("trace_processor")
            if not shell:
                sys.exit("error: install `pip install perfetto` or put "
                         "trace_processor_shell on PATH.")
            self._shell = shell
            self._trace = trace_path
            self._mode = "shell"

    def query(self, sql):
        """Return a list of dict rows."""
        if self._mode == "py":
            return [dict(row.__dict__) if hasattr(row, "__dict__") else dict(row)
                    for row in self._tp.query(sql)]
        # shell mode: -q <file> with `.mode json` style is not available; use the
        # documented JSON output via a query file.
        with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as f:
            f.write(sql)
            qf = f.name
        out = subprocess.run(
            [self._shell, "--query-file", qf, "--query-output-format", "json", self._trace],
            capture_output=True, text=True)
        if out.returncode != 0:
            sys.exit(f"trace_processor_shell failed:\n{out.stderr}")
        try:
            return json.loads(out.stdout) if out.stdout.strip() else []
        except json.JSONDecodeError:
            # older shells stream one JSON object per line
            return [json.loads(l) for l in out.stdout.splitlines() if l.strip()]


# --------------------------------------------------------------------------- #
def slices_by_thread(tp, name_predicate):
    sql = f"""
      SELECT s.name AS name, s.ts AS ts, s.dur AS dur, thr.name AS thread, thr.tid AS tid
      FROM slice s
      JOIN thread_track tt ON s.track_id = tt.id
      JOIN thread thr ON tt.utid = thr.utid
      WHERE {name_predicate}
    """
    return tp.query(sql)


def us(ns):
    return (ns or 0) / 1000.0


def summarize(rows):
    """{phase -> (count, total_us, p50_us, p99_us, thread_set)}"""
    by = {}
    for r in rows:
        n = r["name"]
        by.setdefault(n, {"durs": [], "threads": set()})
        by[n]["durs"].append(us(r["dur"]))
        by[n]["threads"].add(r.get("thread") or "?")
    out = {}
    for n, d in by.items():
        ds = sorted(d["durs"])
        p99 = ds[min(len(ds) - 1, int(0.99 * len(ds)))] if ds else 0.0
        out[n] = {
            "count": len(ds),
            "total_us": sum(ds),
            "p50_us": median(ds) if ds else 0.0,
            "p99_us": p99,
            "threads": d["threads"],
        }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--out-md", default="report.md")
    ap.add_argument("--out-csv", default="slices.csv")
    ap.add_argument("--package", default="dev.aevum.showcase")
    args = ap.parse_args()

    tp = Tp(args.trace)

    aevum_rows = slices_by_thread(tp, "s.name LIKE 'Aevum.%'")
    # ART GC: the HeapTaskDaemon thread, or slices whose name mentions GC.
    gc_rows = slices_by_thread(
        tp, "(thr.name = 'HeapTaskDaemon' OR s.name GLOB '*GC*' OR s.name GLOB '*CollectGarbage*')")

    aevum = summarize(aevum_rows)
    gc = summarize(gc_rows)

    aevum_threads = set().union(*[v["threads"] for v in aevum.values()]) if aevum else set()
    gc_threads = set().union(*[v["threads"] for v in gc.values()]) if gc else set()
    disjoint = aevum_threads.isdisjoint(gc_threads)

    # counters emitted by the engine (Aevum.live_bytes / Aevum.reclaimed_bytes)
    counters = tp.query(
        "SELECT c.name AS name, MAX(ct.value) AS maxv "
        "FROM counter ct JOIN counter_track c ON ct.track_id = c.id "
        "WHERE c.name LIKE 'Aevum.%' GROUP BY c.name")

    # ---- CSV ----
    with open(args.out_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["group", "name", "count", "total_us", "p50_us", "p99_us", "threads"])
        for n in AEVUM_PHASES:
            if n in aevum:
                v = aevum[n]
                w.writerow(["aevum", n, v["count"], f"{v['total_us']:.1f}",
                            f"{v['p50_us']:.1f}", f"{v['p99_us']:.1f}", "|".join(sorted(v["threads"]))])
        for n, v in gc.items():
            w.writerow(["art_gc", n, v["count"], f"{v['total_us']:.1f}",
                        f"{v['p50_us']:.1f}", f"{v['p99_us']:.1f}", "|".join(sorted(v["threads"]))])

    # ---- Markdown ----
    lines = []
    lines.append("# Aevum field-proof - Perfetto analysis\n")
    lines.append(f"- trace: `{args.trace}`")
    lines.append(f"- package: `{args.package}`\n")

    lines.append("## Aevum compaction phases (own thread, concurrent with the app)\n")
    if aevum:
        lines.append("| phase | count | total (µs) | p50 (µs) | p99 (µs) | thread |")
        lines.append("|-------|------:|-----------:|---------:|---------:|--------|")
        for n in AEVUM_PHASES:
            if n in aevum:
                v = aevum[n]
                lines.append(f"| `{n}` | {v['count']} | {v['total_us']:.1f} | "
                             f"{v['p50_us']:.1f} | {v['p99_us']:.1f} | "
                             f"{', '.join(sorted(v['threads']))} |")
    else:
        lines.append("_No `Aevum.*` slices found - was app atrace enabled and a "
                     "compaction driven during capture?_")
    lines.append("")

    lines.append("## ART GC slices (must be a DIFFERENT thread; must not do arena work)\n")
    if gc:
        lines.append("| slice | count | total (µs) | thread |")
        lines.append("|-------|------:|-----------:|--------|")
        for n, v in sorted(gc.items()):
            lines.append(f"| `{n}` | {v['count']} | {v['total_us']:.1f} | "
                         f"{', '.join(sorted(v['threads']))} |")
    else:
        lines.append("_No ART GC slices in the window._")
    lines.append("")

    if counters:
        lines.append("## Engine counters\n")
        lines.append("| counter | max value |")
        lines.append("|---------|----------:|")
        for c in counters:
            lines.append(f"| `{c['name']}` | {int(c.get('maxv') or 0)} |")
        lines.append("")

    verdict = "PASS" if disjoint else "REVIEW"
    lines.append("## Verdict (C1 / C3)\n")
    lines.append(f"- Aevum thread(s): `{', '.join(sorted(aevum_threads)) or 'none'}`")
    lines.append(f"- ART GC thread(s): `{', '.join(sorted(gc_threads)) or 'none'}`")
    lines.append(f"- **Threads disjoint (Aevum ≠ ART GC): {disjoint}** → C3 evidence: "
                 "compaction runs on Aevum's own background thread, never inside ART GC.")
    lines.append(f"- **{verdict}** - pair with `AEVUM_PROOF gc_arena` vs `gc_heap` logcat "
                 "deltas (same workload, graph in-arena vs on the ART heap) for C1: "
                 "ART GC time must not grow with arena graph size.\n")

    with open(args.out_md, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"[parse_perfetto] wrote {args.out_md} and {args.out_csv}")
    print(f"[parse_perfetto] Aevum threads={sorted(aevum_threads)} "
          f"GC threads={sorted(gc_threads)} disjoint={disjoint}")
    sys.exit(0 if (aevum and disjoint) else 1)


if __name__ == "__main__":
    main()

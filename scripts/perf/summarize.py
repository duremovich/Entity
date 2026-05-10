"""
summarize.py — Convert Tracy csvexport output to a structured JSON summary.

Usage:
    python scripts/perf/summarize.py \
        --cpu-csv  <basename>-cpu.csv  \
        --gpu-csv  <basename>-gpu.csv  \
        --plot-csv <basename>-plot.csv \
        --scenario <scenario-name>     \
        [--tracy-file <path.tracy>]    \
        [--duration <seconds>]

Output JSON is written to stdout. Redirect to a file as needed.

CSV inputs are produced by tracy-csvexport:
    tracy-csvexport -u          file.tracy > cpu.csv
    tracy-csvexport -u -p       file.tracy > plot.csv   (plot rows have empty src_file)
    tracy-csvexport -g          file.tracy > gpu.csv

Frame timing is not exported by tracy-csvexport; use Engine::update p95 as
editor-frame proxy and CompositorSystem::update p95 as show-frame proxy.
"""

import argparse
import csv
import json
import statistics
import sys
from datetime import datetime, timezone


# ---------------------------------------------------------------------------
# Stats helpers
# ---------------------------------------------------------------------------

def _percentile(sorted_data, pct):
    """Return the p-th percentile of a pre-sorted list (0-100 scale)."""
    if not sorted_data:
        return None
    n = len(sorted_data)
    idx = (pct / 100.0) * (n - 1)
    lo = int(idx)
    hi = min(lo + 1, n - 1)
    frac = idx - lo
    return sorted_data[lo] + frac * (sorted_data[hi] - sorted_data[lo])


def _zone_stats(values_ns):
    """Return count + median/p95/p99 in ms, rounded to 1 decimal. Null on empty."""
    if not values_ns:
        return {"count": 0, "median_ms": None, "p95_ms": None, "p99_ms": None}
    values_ms = sorted(v / 1e6 for v in values_ns)
    return {
        "count":     len(values_ms),
        "median_ms": round(statistics.median(values_ms), 1),
        "p95_ms":    round(_percentile(values_ms, 95), 1),
        "p99_ms":    round(_percentile(values_ms, 99), 1),
    }


def _coerce(v):
    """Return int when v is a whole number, float otherwise."""
    return int(v) if v == int(v) else v


def _plot_stats(values):
    """Return median/p5/p95 of raw plot values, rounded to 1 decimal. Null on empty."""
    if not values:
        return {"median": None, "p5": None, "p95": None}
    s = sorted(values)
    return {
        "median": _coerce(round(statistics.median(s), 1)),
        "p5":     _coerce(round(_percentile(s, 5), 1)),
        "p95":    _coerce(round(_percentile(s, 95), 1)),
    }


# ---------------------------------------------------------------------------
# CSV readers
# ---------------------------------------------------------------------------

def read_cpu_csv(path):
    """
    Parse -u (per-invocation) CSV. Returns dict[zone_name -> [exec_time_ns]].
    Plot rows (src_file == "") are silently skipped — use read_plot_csv for those.
    """
    zones = {}
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            src_file = row.get("src_file", "").strip()
            if not src_file:
                continue  # plot row mixed in; ignore
            name = row.get("name", "").strip()
            if not name:
                continue
            try:
                ns = float(row["exec_time_ns"])
            except (KeyError, ValueError):
                continue
            zones.setdefault(name, []).append(ns)
    return zones


def read_plot_csv(path):
    """
    Parse -u -p CSV. Returns dict[plot_name -> [value]].
    Plot rows have empty src_file; regular zone rows are skipped.
    """
    plots = {}
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            src_file = row.get("src_file", "").strip()
            if src_file:
                continue  # zone row; skip
            name = row.get("name", "").strip()
            if not name:
                continue
            try:
                val = float(row["value"])
            except (KeyError, ValueError):
                continue
            plots.setdefault(name, []).append(val)
    return plots


def read_gpu_csv(path):
    """
    Parse -g GPU zone CSV. Returns dict[zone_name -> [exec_time_ns]].
    Header columns: name, src_file, Time from start of program, GPU execution time
    """
    GPU_TIME_COL = "GPU execution time"
    zones = {}
    with open(path, newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f)
        for row in reader:
            name = row.get("name", "").strip()
            if not name:
                continue
            try:
                ns = float(row[GPU_TIME_COL])
            except (KeyError, ValueError):
                continue
            zones.setdefault(name, []).append(ns)
    return zones


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def build_summary(cpu_path, gpu_path, plot_path, scenario, tracy_file, duration_seconds):
    cpu_raw  = read_cpu_csv(cpu_path)
    gpu_raw  = read_gpu_csv(gpu_path)
    plot_raw = read_plot_csv(plot_path)

    cpu_zones = {name: _zone_stats(vals) for name, vals in cpu_raw.items()}
    gpu_zones = {name: _zone_stats(vals) for name, vals in gpu_raw.items()}
    plots     = {name: _plot_stats(vals) for name, vals in plot_raw.items()}

    return {
        "scenario":        scenario,
        "captured_at":     datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "duration_seconds": duration_seconds,
        "tracy_file":      tracy_file or "",
        "cpu_zones":       cpu_zones,
        "gpu_zones":       gpu_zones,
        "plots":           plots,
        "notes": (
            "Frame timing not exported by tracy-csvexport; use Engine::update p95 as "
            "editor-frame proxy and CompositorSystem::update p95 as show-frame proxy."
        ),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Convert Tracy csvexport CSVs to a structured JSON perf summary."
    )
    parser.add_argument("--cpu-csv",    required=True, help="CPU zone CSV (tracy-csvexport -u)")
    parser.add_argument("--gpu-csv",    required=True, help="GPU zone CSV (tracy-csvexport -g)")
    parser.add_argument("--plot-csv",   required=True, help="Plot CSV (tracy-csvexport -u -p)")
    parser.add_argument("--scenario",   required=True, help="Scenario name (e.g. stress_3layer_prores_1080p)")
    parser.add_argument("--tracy-file", default="",    help="Path to the .tracy capture file")
    parser.add_argument("--duration",   type=float, default=30.0, help="Capture duration in seconds (default: 30)")
    args = parser.parse_args()

    summary = build_summary(
        cpu_path       = args.cpu_csv,
        gpu_path       = args.gpu_csv,
        plot_path      = args.plot_csv,
        scenario       = args.scenario,
        tracy_file     = args.tracy_file,
        duration_seconds = args.duration,
    )
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Summarize paired OFF/ADAPTIVE field-IC benchmark artifacts."""

import json
import math
import pathlib
import statistics
import sys


T95 = {7: 2.447}


def percentile(values, q):
    ordered = sorted(values)
    return ordered[round((len(ordered) - 1) * q)]


def geometric_mean(values):
    return math.exp(statistics.mean(math.log(value) for value in values))


def main():
    root = pathlib.Path(sys.argv[1])
    runs = {}
    bodies = {}
    reports = {}
    for path in sorted(root.glob("*.jsonl")):
        case, pair, slot, mode = path.stem.split(".")
        rows = [json.loads(line) for line in path.read_text().splitlines()
                if line.strip()]
        if not rows or not all(row["ok"] and row["status"] == 200 for row in rows):
            raise SystemExit(f"invalid result rows: {path}")
        key = (case, int(pair), mode)
        runs.setdefault(key, []).append(statistics.median(row["ms"] for row in rows))
        bodies.setdefault(case, set()).update(row["body"] for row in rows)
        err = path.with_suffix(".err")
        report_rows = []
        for line in err.read_text().splitlines():
            if line.startswith('{"ic_mode"'):
                report_rows.append(json.loads(line))
        if len(report_rows) != 1:
            raise SystemExit(f"expected one IC report: {err}")
        reports.setdefault((case, mode), []).append(report_rows[0])

    print("case pairs paired_gain_95ci same_sign off_ms adaptive_ms "
          "quickened dequickened max_bytes")
    for case in sorted(bodies):
        if len(bodies[case]) != 1:
            raise SystemExit(f"body mismatch across arms: {case}")
        pairs = sorted(pair for name, pair, mode in runs
                       if name == case and mode == "off")
        ratios = []
        off_values = []
        adaptive_values = []
        for pair in pairs:
            off = geometric_mean(runs[(case, pair, "off")])
            adaptive = geometric_mean(runs[(case, pair, "adaptive")])
            off_values.append(off)
            adaptive_values.append(adaptive)
            ratios.append(off / adaptive)
        logs = [math.log(value) for value in ratios]
        center_log = statistics.mean(logs)
        if len(logs) > 1:
            half = T95.get(len(logs), 1.96) * statistics.stdev(logs) / math.sqrt(len(logs))
        else:
            half = 0.0
        gain = (math.exp(center_log) - 1.0) * 100.0
        lo = (math.exp(center_log - half) - 1.0) * 100.0
        hi = (math.exp(center_log + half) - 1.0) * 100.0
        adaptive_reports = reports[(case, "adaptive")]
        quickened = statistics.median(row["quickened"] for row in adaptive_reports)
        dequickened = statistics.median(row["dequickened"] for row in adaptive_reports)
        max_bytes = max(row["bytes"] for row in adaptive_reports)
        same_sign = sum(value > 1.0 for value in ratios)
        print(f"{case} {len(pairs)} {gain:+.2f}%[{lo:+.2f},{hi:+.2f}] "
              f"{same_sign}/{len(pairs)} {statistics.median(off_values):.3f} "
              f"{statistics.median(adaptive_values):.3f} {quickened:g} "
              f"{dequickened:g} {max_bytes}")
    print("correctness: all status/body checks identical across OFF/ADAPTIVE")


if __name__ == "__main__":
    main()

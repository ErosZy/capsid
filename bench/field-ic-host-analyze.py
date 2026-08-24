#!/usr/bin/env python3
"""Summarize the paired host/loadgen field-IC screen."""

import json
import math
import pathlib
import statistics
import sys


def gmean(values):
    return math.exp(statistics.mean(math.log(value) for value in values))


def interval(ratios):
    logs = [math.log(value) for value in ratios]
    center = statistics.mean(logs)
    half = 2.447 * statistics.stdev(logs) / math.sqrt(len(logs))
    return tuple((math.exp(value) - 1.0) * 100.0
                 for value in (center, center - half, center + half))


def main():
    root = pathlib.Path(sys.argv[1])
    runs = {}
    for path in sorted(root.glob("samples.*.jsonl")):
        _, workload, pair, slot, mode = path.stem.split(".")
        rows = [json.loads(line) for line in path.read_text().splitlines()
                if line.strip()]
        measured = [row for row in rows if row["phase"] == "measured"]
        if len(measured) != 1:
            raise SystemExit(f"missing measured row: {path}")
        row = measured[0]
        correctness = path.with_name(
            f"correctness.{workload}.{pair}.{slot}.{mode}.json")
        verdict = json.loads(correctness.read_text())
        if (not verdict["ok"] or verdict["mismatches"] or
                verdict["errors"] or verdict["timeouts"] or
                row["errors"] or row["timeouts"]):
            raise SystemExit(f"correctness failure: {path}")
        runs.setdefault((workload, int(pair), mode), []).append(row)

    print("workload pairs qps_gain_95ci qps_same_sign p95_change_95ci")
    workloads = sorted({key[0] for key in runs})
    for workload in workloads:
        pairs = sorted({key[1] for key in runs if key[0] == workload})
        qps_ratios = []
        p95_ratios = []
        for pair in pairs:
            off = runs[(workload, pair, "off")]
            adaptive = runs[(workload, pair, "adaptive")]
            qps_ratios.append(gmean(row["qps"] for row in adaptive) /
                              gmean(row["qps"] for row in off))
            p95_ratios.append(gmean(row["p95_ms"] for row in adaptive) /
                              gmean(row["p95_ms"] for row in off))
        qps = interval(qps_ratios)
        p95 = interval(p95_ratios)
        signs = sum(value > 1.0 for value in qps_ratios)
        print(f"{workload} {len(pairs)} {qps[0]:+.2f}%"
              f"[{qps[1]:+.2f},{qps[2]:+.2f}] {signs}/{len(pairs)} "
              f"{p95[0]:+.2f}%[{p95[1]:+.2f},{p95[2]:+.2f}]")
    print("correctness: every loadgen verdict clean")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Summarize patchless versus a feature-built worker's latency."""

import json
import math
import pathlib
import statistics
import sys


def gmean(values):
    return math.exp(statistics.mean(math.log(value) for value in values))


def main():
    root = pathlib.Path(sys.argv[1])
    runs = {}
    bodies = {}
    for path in root.glob("*.jsonl"):
        case, pair, slot, mode = path.stem.split(".")
        rows = [json.loads(line) for line in path.read_text().splitlines()
                if line.strip()]
        if not rows or not all(row["ok"] and row["status"] == 200 for row in rows):
            raise SystemExit(f"invalid rows: {path}")
        runs.setdefault((case, int(pair), mode), []).append(
            statistics.median(row["ms"] for row in rows))
        bodies.setdefault(case, set()).update(row["body"] for row in rows)
    labels = {key[2] for key in runs}
    feature_labels = labels - {"patchless"}
    if len(feature_labels) != 1:
        raise SystemExit(f"expected one feature label, got {sorted(feature_labels)}")
    feature_label = feature_labels.pop()
    pair_ids = sorted({key[1] for key in runs})
    print(f"case pairs {feature_label}_vs_patchless_95ci regress_sign")
    for case in sorted(bodies):
        if len(bodies[case]) != 1:
            raise SystemExit(f"body mismatch: {case}")
        ratios = []
        for pair in pair_ids:
            patchless = gmean(runs[(case, pair, "patchless")])
            feature = gmean(runs[(case, pair, feature_label)])
            ratios.append(feature / patchless)
        logs = [math.log(value) for value in ratios]
        center = statistics.mean(logs)
        if len(logs) < 2:
            raise SystemExit("at least two pairs are required")
        # Student-t 95% critical values for the common benchmark sizes; use
        # the normal approximation once the sample is larger than the table.
        t95 = {2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571,
               7: 2.447, 8: 2.365, 9: 2.306, 10: 2.262,
               11: 2.228, 12: 2.201, 13: 2.179, 14: 2.160,
               15: 2.145, 16: 2.131, 17: 2.120, 18: 2.110,
               19: 2.101, 20: 2.093, 21: 2.086}
        critical = t95.get(len(logs), 1.96)
        half = critical * statistics.stdev(logs) / math.sqrt(len(logs))
        pct = lambda value: (math.exp(value) - 1.0) * 100.0
        print(f"{case} {len(logs)} {pct(center):+.2f}%"
              f"[{pct(center-half):+.2f},{pct(center+half):+.2f}] "
              f"{sum(value > 1.0 for value in ratios)}/{len(ratios)}")
    print("correctness: bodies identical")


if __name__ == "__main__":
    main()

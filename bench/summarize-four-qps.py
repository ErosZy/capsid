#!/usr/bin/env python3
"""Summarize a bench/compare-four-qps.sh output directory.

Reads the raw files only (samples.*.jsonl, correctness.*.json,
resources.jsonl, manifest.txt, progress.log) and prints:
  - per (side, workload) median QPS + p50/p95/p99 over rounds;
  - per-process resource table (median PSS/RSS/CPU over the sampling
    window; capsid host and worker listed separately);
  - correctness verdicts and any FAILED lines from progress.log.
"""
import glob
import json
import os
import re
import statistics
import sys


def median(values):
    return statistics.median(values) if values else 0.0


def main(out):
    samples = {}
    for path in sorted(glob.glob(os.path.join(out, "samples.*.jsonl"))):
        base = os.path.basename(path)
        # samples.<side>.<workload>.c<conns>.r<round>.jsonl
        parts = base.split(".")
        if len(parts) < 5:
            continue
        side, workload = parts[1], parts[2]
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                row = json.loads(line)
                if row.get("phase") != "measured":
                    continue
                samples.setdefault((side, workload), []).append(row)

    sides = ["capsid", "php", "flask", "fastapi"]
    workloads = sorted({w for (_, w) in samples})
    workloads = sorted(workloads, key=lambda w: (
        {"json": 0, "bytes": 1, "stream": 2}.get(w.rstrip("0123456789k"), 3),
        int(re.search(r"\d+", w).group()) if re.search(r"\d+", w) else 0,
        w))
    if not workloads:
        print("no measured samples found in " + out)
        return 1

    width = 40
    hdr = "stack".ljust(10) + "workload".ljust(16) + "QPS".rjust(9) + \
          "CV%".rjust(7) + "p50_ms".rjust(9) + "p95_ms".rjust(9) + "p99_ms".rjust(9) + "err".rjust(5)
    print(hdr)
    print("-" * len(hdr))
    ranking = {}
    for workload in workloads:
        best = None
        for side in sides:
            rows = samples.get((side, workload), [])
            if not rows:
                print(f"{side:<10}{workload:<16}{'n/a':>9}{'':>7}{'':>9}{'':>9}{'':>9}{'':>5}")
                continue
            qps = median(r["qps"] for r in rows)
            cv = (statistics.pstdev(r["qps"] for r in rows) /
                  statistics.fmean(r["qps"] for r in rows) * 100) if len(rows) > 1 else 0.0
            p50 = median(r["p50_ms"] for r in rows)
            p95 = median(r["p95_ms"] for r in rows)
            p99 = median(r["p99_ms"] for r in rows)
            errs = sum(r["errors"] + r["timeouts"] for r in rows)
            if best is None or qps > best[0]:
                best = (qps, side)
            marker = ""
            print(f"{side:<10}{workload:<16}{qps:>9.0f}{cv:>7.1f}{p50:>9.2f}{p95:>9.2f}{p99:>9.2f}{errs:>5}")
        if best:
            ranking.setdefault(workload, []).append(best)

    print()
    print("QPS ranking (median over rounds):")
    for workload, order in ranking.items():
        order.sort(reverse=True)
        print(f"  {workload:<12} " + " | ".join(f"{s}: {q:.0f}" for q, s in order))

    # --- correctness ---
    bad = []
    for path in sorted(glob.glob(os.path.join(out, "correctness.*.json"))):
        with open(path, encoding="utf-8") as handle:
            verdict = json.load(handle)
        if not verdict.get("ok"):
            bad.append(os.path.basename(path) + " " + verdict.get("error", "not ok"))
    print()
    if bad:
        print(f"correctness: {len(bad)} FAILURES")
        for line in bad[:20]:
            print("  " + line)
    else:
        total = len(glob.glob(os.path.join(out, "correctness.*.json")))
        print(f"correctness: all {total} rounds OK")

    # --- resources (per process role) ---
    res_path = os.path.join(out, "resources.jsonl")
    if os.path.exists(res_path):
        roles = {}
        with open(res_path, encoding="utf-8") as handle:
            for line in handle:
                row = json.loads(line)
                roles.setdefault(row["role"], []).append(row)
        print()
        print("per-process resources (median over sampling window;"
              " CPU = window-average % of one core)")
        print("role".ljust(18) + "pids".rjust(6) + "PSS_MB".rjust(9) +
              "RSS_MB".rjust(9) + "CPU%".rjust(8) + "CPU_max".rjust(9))
        for role in sorted(roles):
            rows = roles[role]
            pss = [r["pss_kb"] / 1024 for r in rows if r["pss_kb"] is not None]
            rss = [r["rss_kb"] / 1024 for r in rows if r["rss_kb"] is not None]
            cpu = [r["cpu_pct"] for r in rows]
            nonzero_cpu = [c for c in cpu if c > 0.1]
            # per-process, not per-sample: aggregate by pid first
            by_pid = {}
            for r in rows:
                by_pid.setdefault(r["pid"], []).append(r)
            pid_pss = [median([x["pss_kb"] for x in v if x["pss_kb"] is not None]) / 1024
                       for v in by_pid.values()]
            pid_rss = [median([x["rss_kb"] for x in v if x["rss_kb"] is not None]) / 1024
                       for v in by_pid.values()]
            pid_cpu = [median([x["cpu_pct"] for x in v]) for v in by_pid.values()]
            pid_cpu_max = [max(x["cpu_pct"] for x in v) for v in by_pid.values()]
            agg = lambda xs: median(xs) if xs else 0.0
            print(f"{role:<18}{len(by_pid):>6}{agg(pid_pss):>9.1f}{agg(pid_rss):>9.1f}"
                  f"{agg(pid_cpu):>8.1f}{max(pid_cpu_max):>9.0f}")
    else:
        print("\nno resources.jsonl (sampler not run)")

    # --- failures from progress ---
    if os.path.exists(os.path.join(out, "progress.log")):
        failed = [l for l in open(os.path.join(out, "progress.log"), encoding="utf-8")
                  if "FAILED" in l]
        if failed:
            print()
            print(f"{len(failed)} loadgen FAILED lines:")
            for line in failed[:20]:
                print("  " + line.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

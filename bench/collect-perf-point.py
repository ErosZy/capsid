#!/usr/bin/env python3
"""Collect benchmark result directories into one perf-site data point.

Reads a cold-start runner output dir (bench/cold-start.sh) and a QPS lane
output dir (bench/qps-lane.sh), extracts medians, and merges the point into
perf-site/data.js (schema 1). Existing points with the same version are
replaced; the file stays append-only across versions.

Usage:
  python3 bench/collect-perf-point.py \
      --cold-start <dir> --qps <dir> \
      --version "0.2.x" --release "v0.2.2" \
      [--data perf-site/data.js]
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys

SIZES = ["10k", "100k", "1m"]
STACKS = ["capsid-source", "capsid-bytecode", "node", "deno", "workerd"]


def median(values):
    vs = sorted(v for v in values if v is not None)
    if not vs:
        return None
    return vs[len(vs) // 2]


def parse_cold_start(path):
    point = {"coldStart": {"cells": [], "warm": [], "pssMb": []}}
    seen = {}
    with open(os.path.join(path, "cells.txt")) as f:
        for line in f:
            m = re.match(
                r"(\S+)\s+(\S*)\s+(\S+)\s+total_ms=\s*([\d.]+)\s+"
                r"ready_ms=\s*([\d.]+)(?:\s+warm_ms=\s*([\d.]+))?"
                r"(?:\s+pss_mb=(\S+))?",
                line.strip(),
            )
            if not m:
                continue
            stack, mode, size, total, ready, warm, pss = m.groups()
            name = stack if stack != "capsid" else "capsid-" + mode
            key = (name, size)
            if key in seen:
                raise SystemExit(
                    f"{path}/cells.txt: duplicate cold cell {key} "
                    "(same stack measured twice in one point); keep exactly "
                    "one same-origin, same-config record per cell")
            seen[key] = True
            point["coldStart"]["cells"].append({
                "stack": name,
                "size": size,
                "totalMs": float(total),
                "readyMs": float(ready),
                "warmMs": float(warm) if warm else None,
                "pssMb": float(pss) if pss not in (None, "n/a") else None,
            })
    return point["coldStart"]


def parse_qps(path):
    out = {"cells": []}
    with open(os.path.join(path, "cells.txt")) as f:
        for line in f:
            m = re.match(
                r"(\S+)\s+(\S*)\s+qps_med=\s*([\d.]+)"
                r"(?:\s+p50_ms=([\d.]+))?(?:\s+p95_ms=([\d.]+))?",
                line.strip(),
            )
            if m:
                stack, wl, qps, p50, p95 = m.groups()
                out["cells"].append({
                    "stack": stack,
                    "workload": wl,
                    "qps": float(qps),
                    "p50Ms": float(p50) if p50 else None,
                    "p95Ms": float(p95) if p95 else None,
                })
                continue
            m = re.match(r"(\S+)\s+serving_pss_mb=(\S+)", line.strip())
            if m:
                out.setdefault("servingPssMb", {})[m.group(1)] = float(m.group(2))
                continue
            # serving_pss_detail=<stack> role=mb;role=mb...
            m = re.match(r"(\S+)\s+serving_pss_detail=(\S+)", line.strip())
            if m:
                stack, pairs = m.groups()
                parts = _parse_pairs(pairs)
                if parts:
                    out.setdefault("servingPss", {})[stack] = parts
                continue
            m = re.match(r"(\S+)\s+lowqps_pss_mb=(\S+)", line.strip())
            if m:
                out.setdefault("lowLoadPssMb", {})[m.group(1)] = float(m.group(2))
                continue
            m = re.match(r"(\S+)\s+lowqps_pss_detail=(\S+)", line.strip())
            if m:
                parts = _parse_pairs(m.group(2))
                if parts:
                    out.setdefault("lowLoadPss", {})[m.group(1)] = parts
    return out


def _parse_pairs(pairs):
    parts = []
    for pair in pairs.split(";"):
        if "=" in pair:
            role, _, mb = pair.partition("=")
            try:
                parts.append({"role": role, "mb": float(mb)})
            except ValueError:
                pass
    return parts


def machine_fingerprint(paths):
    fp = {}
    for path in paths:
        mf = os.path.join(path, "manifest.txt")
        if not os.path.exists(mf):
            continue
        with open(mf) as f:
            for line in f:
                if ":" not in line:
                    continue
                key, _, value = line.partition(":")
                key = key.strip()
                value = value.strip()
                if key in ("cpu", "uname", "nproc", "workerd_version",
                           "node", "deno", "commit", "rounds", "warm",
                           "command", "generated_at", "ab",
                           "php_fpm", "nginx", "python", "python_pkgs",
                           "php_pool"):
                    fp.setdefault(key, value)
    return fp


def evidence_files(paths):
    files = {}
    for path in paths:
        sf = os.path.join(path, "sha256sums.txt")
        if not os.path.exists(sf):
            continue
        with open(sf) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    files[os.path.join(os.path.basename(path),
                                       os.path.basename(parts[1]))] = parts[0]
    return files


def git_info():
    def run(*args):
        try:
            return subprocess.check_output(
                ["git", *args], text=True, stderr=subprocess.DEVNULL).strip()
        except subprocess.CalledProcessError:
            return ""
    return {"commit": run("rev-parse", "HEAD"),
            "tag": run("describe", "--exact-match", "--tags", "HEAD")}


def load_existing(data_path):
    if not os.path.exists(data_path):
        return None
    with open(data_path) as f:
        content = f.read()
    m = re.search(r"window\.CAPSID_PERF_DATA\s*=\s*(\{.*\})\s*;?\s*$",
                  content, re.S)
    if not m:
        return None
    return json.loads(m.group(1))


def parse_density(path):
    """cells.txt lines: `capsid 10k workers=17 pss_mb=47.8 note=...`"""
    out = {}
    if not path:
        return out
    fp = os.path.join(path, "cells.txt")
    if not os.path.exists(fp):
        return out
    with open(fp) as f:
        for line in f:
            m = re.match(r"(\S+)\s+(\S+)\s+(workers|cells)=(\d+)\s+"
                         r"pss_mb=(\S+)", line.strip())
            if not m:
                continue
            stack, size, unit, count, pss = m.groups()
            try:
                pss = float(pss)
            except ValueError:
                continue
            out.setdefault(stack, {})[size] = {
                "count": int(count), "unit": unit, "pssMb": pss}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cold-start", required=True)
    ap.add_argument("--qps", required=True)
    ap.add_argument("--density")
    ap.add_argument("--version", required=True)
    ap.add_argument("--release", required=True)
    ap.add_argument("--data", default="perf-site/data.js")
    args = ap.parse_args()

    cold = parse_cold_start(args.cold_start)
    qps = parse_qps(args.qps)
    machine = machine_fingerprint([args.cold_start, args.qps])

    # Environment cross-checks: a point may not carry results for a stack
    # whose identity the fingerprint cannot corroborate.
    cold_stack_names = {c["stack"] for c in cold["cells"]}
    for stack, fp_key in (("deno", "deno"), ("node", "node"),
                          ("workerd", "workerd_version")):
        identity = machine.get(fp_key)
        if stack in cold_stack_names and (not identity or identity == "absent"):
            raise SystemExit(
                f"cold-start cells contain {stack} results but the "
                f"fingerprint lacks a {fp_key} identity")

    point = {
        "schema": 1,
        "version": args.version,
        "release": args.release,
        "date": datetime.date.today().isoformat(),
        "notes": os.environ.get("CAPSID_PERF_NOTES", ""),
        "git": git_info(),
        "machine": machine,
        "coldStart": cold,
        "qps": qps,
        "density": parse_density(args.density),
        "evidence": evidence_files([args.cold_start, args.qps]),
    }

    existing = load_existing(args.data) or {"schema": 1, "points": []}
    existing["points"] = [p for p in existing["points"]
                          if p["version"] != args.version]
    existing["points"].append(point)
    existing["points"].sort(key=lambda p: p["date"])

    with open(args.data, "w") as f:
        f.write("// Generated by bench/collect-perf-point.py — append-only "
                "performance history.\n")
        f.write("// Each point is tied to its release, git commit, machine "
                "fingerprint, and\n// evidence SHA-256 digests. Do not edit "
                "historical points.\n")
        f.write("window.CAPSID_PERF_DATA = ")
        json.dump(existing, f, indent=1, sort_keys=True)
        f.write(";\n")
    print("wrote %s (%d points)" % (args.data, len(existing["points"])))


if __name__ == "__main__":
    main()

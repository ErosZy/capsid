#!/usr/bin/env python3
"""Publish complete, sealed performance archives; parse_* helpers are independent.

The CLI always applies the publication contract. Simplified parser fixtures
cannot produce qualified points. See bench/perf-evidence.md for lane.json.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from perf_evidence import EvidenceError, object_pairs, sha256, timestamp, validate_point


def number(value, label):
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise EvidenceError(f"{label}: invalid number {value!r}") from exc
    if not math.isfinite(parsed) or parsed < 0:
        raise EvidenceError(f"{label}: expected a finite nonnegative number")
    return parsed


def lines(path):
    file = Path(path) / "cells.txt"
    for index, line in enumerate(file.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if line and not line.startswith("#"):
            yield f"{file}:{index}", line


def fields(value, label):
    result = {}
    rest = value.strip()
    while rest:
        match = re.match(r"([a-z0-9_]+)=\s*(\S+)(?:\s+|$)", rest)
        if not match or match[1] in result:
            raise EvidenceError(f"{label}: malformed or duplicate field")
        result[match[1]] = match[2]
        rest = rest[match.end():]
    return result


def unique(seen, key, label):
    if key in seen:
        raise EvidenceError(f"{label}: duplicate cell {key}")
    seen.add(key)


def parse_cold_start(path):
    out, seen = {"cells": [], "warm": [], "pssMb": []}, set()
    for label, line in lines(path):
        head, sep, tail = line.partition("total_ms=")
        words = head.split()
        if not sep or not (len(words) == 3 and words[0] == "capsid" or
                           len(words) == 2 and words[0] != "capsid"):
            raise EvidenceError(f"{label}: malformed cold-start row")
        if words[0] == "capsid":
            mode = "source" if words[1] == "src" else words[1]
            stack, size = "capsid-" + mode, words[2]
        else:
            stack, size = words
        unique(seen, (stack, size), label)
        values = fields("total_ms=" + tail, label)
        if not {"total_ms", "ready_ms"} <= values.keys() <= {
                "total_ms", "ready_ms", "warm_ms", "pss_mb"}:
            raise EvidenceError(f"{label}: missing or unknown cold-start fields")
        cell = {"stack": stack, "size": size}
        for source, target in (("total_ms", "totalMs"), ("ready_ms", "readyMs"),
                               ("warm_ms", "warmMs"), ("pss_mb", "pssMb")):
            value = values.get(source)
            cell[target] = (None if value is None or source == "pss_mb" and value == "n/a"
                            else number(value, label))
        out["cells"].append(cell)
    return out


def _parse_pairs(pairs, label="PSS breakdown"):
    parts, seen = [], set()
    for pair in pairs.split(";"):
        role, sep, value = pair.partition("=")
        if not sep or not role:
            raise EvidenceError(f"{label}: malformed PSS breakdown")
        unique(seen, role, label)
        parts.append({"role": role, "mb": number(value, label)})
    return parts


def parse_qps(path):
    out, seen = {"cells": []}, set()
    memory = {"serving_pss_mb": "servingPssMb", "serving_pss_detail": "servingPss",
              "lowqps_pss_mb": "lowLoadPssMb", "lowqps_pss_detail": "lowLoadPss"}
    for label, line in lines(path):
        words = line.split(None, 2)
        if len(words) == 3 and words[2].startswith("qps_med="):
            stack, workload, tail = words
            unique(seen, ("qps", stack, workload), label)
            values = fields(tail, label)
            if values.keys() != {"qps_med", "p50_ms", "p95_ms"}:
                raise EvidenceError(f"{label}: missing or unknown QPS fields")
            out["cells"].append({"stack": stack, "workload": workload,
                                  "qps": number(values["qps_med"], label),
                                  "p50Ms": number(values["p50_ms"], label),
                                  "p95Ms": number(values["p95_ms"], label)})
            continue
        match = re.fullmatch(r"(\S+)\s+(\w+)=(\S+)", line)
        if not match or match[2] not in memory:
            raise EvidenceError(f"{label}: malformed QPS or PSS row")
        stack, key, value = match.groups()
        unique(seen, (key, stack), label)
        out.setdefault(memory[key], {})[stack] = (
            _parse_pairs(value, label) if key.endswith("detail") else number(value, label))
    return out


def parse_density(path):
    out, seen = {}, set()
    if path is None:
        return out
    for label, line in lines(path):
        match = re.fullmatch(r"(\S+)\s+(\S+)\s+(workers|cells)=(\d+)\s+"
                             r"pss_mb=(\S+)(?:\s+phase=(cold|warm))?(?:\s+note=(.*))?", line)
        if not match:
            raise EvidenceError(f"{label}: malformed density row")
        stack, size, unit, count, pss, phase, _ = match.groups()
        phase = phase or "warm"  # Parsing old syntax does not qualify publication.
        unique(seen, (stack, size, phase), label)
        if int(count) <= 0 or unit != ("workers" if stack == "capsid" else "cells"):
            raise EvidenceError(f"{label}: invalid density count or unit")
        out.setdefault(stack, {})[phase + "@" + size] = {
            "count": int(count), "unit": unit, "pssMb": number(pss, label)}
    return out


def git_info(repo):
    def run(*args):
        try:
            return subprocess.check_output(["git", "-C", str(repo), *args], text=True,
                                           stderr=subprocess.DEVNULL).strip()
        except (OSError, subprocess.CalledProcessError):
            return ""
    return {"commit": run("rev-parse", "HEAD"),
            "tag": run("describe", "--exact-match", "--tags", "HEAD")}


def load_existing(data_path):
    if not data_path.exists():
        return {"schema": 1, "points": []}
    content = data_path.read_text(encoding="utf-8")
    match = re.search(r"window\.CAPSID_PERF_DATA\s*=\s*(\{.*\})\s*;?\s*$", content, re.S)
    if not match:
        raise EvidenceError(f"{data_path}: invalid history wrapper; refusing to overwrite")
    data = json.loads(match[1], object_pairs_hook=object_pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(
                          EvidenceError(f"{data_path}: non-finite JSON {value}")))
    if not isinstance(data, dict) or data.get("schema") != 1 or not isinstance(data.get("points"), list):
        raise EvidenceError(f"{data_path}: unsupported history schema")
    versions = set()
    for point in data["points"]:
        if not isinstance(point, dict) or not isinstance(point.get("version"), str) or not isinstance(point.get("date"), str):
            raise EvidenceError(f"{data_path}: invalid historical point")
        unique(versions, point["version"], str(data_path))
    return data


def write_data(path, data):
    # Serialize before touching the destination; failure preserves history.
    content = ("// Generated by bench/collect-perf-point.py. Do not edit numbers.\n"
               "window.CAPSID_PERF_DATA = " +
               json.dumps(data, indent=1, sort_keys=True, allow_nan=False) + ";\n")
    temp = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n",
                                         dir=path.parent, prefix=".perf-data-", delete=False) as stream:
            temp = Path(stream.name)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        if temp is not None and temp.exists():
            temp.unlink()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cold-start", required=True)
    ap.add_argument("--qps", required=True)
    ap.add_argument("--density")
    ap.add_argument("--version", required=True)
    ap.add_argument("--release", required=True)
    ap.add_argument("--data", default="perf-site/data.js")
    ap.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                    help="archive root; lane directories must be under its bench/results")
    ap.add_argument("--generated-at", required=True, help="explicit generation time with timezone")
    ap.add_argument("--check", action="store_true", help="validate without writing data.js")
    args = ap.parse_args(argv)
    generated = timestamp(args.generated_at, "generated-at")
    directories = {"cold-start": args.cold_start, "qps": args.qps}
    if args.density is not None:
        directories["density"] = args.density
    summary_hashes = {lane: sha256(Path(path) / "cells.txt") for lane, path in directories.items()}
    cold, qps, density = parse_cold_start(args.cold_start), parse_qps(args.qps), parse_density(args.density)
    lanes = {"cold-start": (args.cold_start, cold), "qps": (args.qps, qps)}
    if args.density is not None:
        lanes["density"] = args.density, density
    provenance = validate_point(lanes, args.repo, args.version, args.release)
    for lane, path in directories.items():
        prefix = provenance["verification"]["archives"][lane]
        if summary_hashes[lane] != provenance["evidence"][prefix + "/cells.txt"]:
            raise EvidenceError(f"{lane}: summary changed during parsing")
    point = {"schema": 1, "version": args.version, "release": args.release,
             "notes": os.environ.get("CAPSID_PERF_NOTES", ""), "coldStart": cold,
             "qps": qps, "density": density, **provenance,
             "generation": {"at": generated.isoformat().replace("+00:00", "Z"),
                            "collector": git_info(args.repo),
                            "implementation": {name: sha256(Path(__file__).parent / name)
                                               for name in ("collect-perf-point.py", "perf_evidence.py")}}}
    if args.check:
        print(json.dumps({"status": "verified", "evidenceFiles": len(point["evidence"]),
                          "measurementCommit": point["git"]["commit"]}, sort_keys=True))
        return
    path = Path(args.data)
    existing = load_existing(path)
    existing["points"] = [p for p in existing["points"] if p["version"] != args.version]
    existing["points"].append(point)
    existing["points"].sort(key=lambda p: (p["date"], p["version"]))
    write_data(path, existing)
    print(f"wrote {path} ({len(existing['points'])} points)")


if __name__ == "__main__":
    try:
        main()
    except (EvidenceError, OSError, ValueError, TypeError, KeyError) as exc:
        print(f"performance evidence rejected: {exc}", file=sys.stderr)
        sys.exit(1)

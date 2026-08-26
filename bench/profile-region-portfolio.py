#!/usr/bin/env python3
"""Run the stable v4 profile→CFG+SSA census over an entire portfolio."""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import resource
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path


CENSUS_RE = re.compile(
    r"census: (\d+) functions, rejected (\d+) functions / (\d+) insns, "
    r"dynamic (yes|no), missing sites (\d+)")
STATIC_RE = re.compile(
    r"bytecode region:\s+(\S+)\s+candidates (\d+), insns (\d+), "
    r"guards (\d+), slow bytes (\d+), predicted (-?\d+) \(best (-?\d+)\)")
DYNAMIC_RE = re.compile(
    r"bytecode region:\s+dynamic regions (\d+), insns (\d+), "
    r"predicted (-?\d+)")


def load_bridge(root: Path):
    path = root / "bench/profile-regions.py"
    spec = importlib.util.spec_from_file_location("capsid_profile_regions", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def parse_report(text: str) -> dict:
    result: dict[str, object] = {"templates": {}}
    current: dict[str, int] | None = None
    for line in text.splitlines():
        census = CENSUS_RE.search(line)
        if census:
            result.update({
                "functions": int(census.group(1)),
                "rejected_functions": int(census.group(2)),
                "rejected_insns": int(census.group(3)),
                "missing_profile_sites": int(census.group(5)),
            })
            continue
        static = STATIC_RE.search(line)
        if static:
            current = {
                "candidates": int(static.group(2)),
                "insns": int(static.group(3)),
                "guards": int(static.group(4)),
                "slow_bytes": int(static.group(5)),
                "predicted": int(static.group(6)),
                "best": int(static.group(7)),
            }
            result["templates"][static.group(1)] = current  # type: ignore[index]
            continue
        dynamic = DYNAMIC_RE.search(line)
        if dynamic and current is not None:
            current.update({
                "dynamic_regions": int(dynamic.group(1)),
                "dynamic_insns": int(dynamic.group(2)),
                "dynamic_predicted": int(dynamic.group(3)),
            })
    if "functions" not in result:
        raise ValueError("analyzer output has no region census")
    return result


def source_names_from_manifest(collection: Path) -> dict[str, str]:
    """Recover the exact compiler source URL instead of guessing its prefix."""
    path = collection / "manifest.json"
    if not path.is_file():
        return {}
    obj = json.loads(path.read_text(encoding="utf-8"))
    programs = obj.get("programs", [])
    results = obj.get("results", [])
    names: dict[str, str] = {}
    for program, result in zip(programs, results):
        stem = result.get("program")
        source_name = result.get("source_name")
        if source_name is None and isinstance(program.get("file"), str):
            source_name = f"file:///{program['file']}"
        if isinstance(stem, str) and isinstance(source_name, str):
            names[stem] = source_name
    return names


def build_output(collection: Path, classic: bool, rows: list[dict]) -> dict:
    aggregate: dict[str, dict[str, int]] = defaultdict(
        lambda: defaultdict(int))
    failures = 0
    for row in rows:
        if "error" in row:
            failures += 1
            continue
        for template, counters in row.get("templates", {}).items():
            for key, value in counters.items():
                if key == "best":
                    aggregate[template][key] = max(
                        aggregate[template].get(key, value), value)
                else:
                    aggregate[template][key] += value
    return {
        "schema": "capsid-profile-region-portfolio-v1",
        "collection": str(collection.resolve()),
        "classic": classic,
        "programs": rows,
        "aggregate": {name: dict(values)
                      for name, values in sorted(aggregate.items())},
        "failures": failures,
    }


def write_output(path: Path, output: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(output, indent=2) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def analyzer_limit(memory_mib: int):
    """Return a child-only address-space limiter for the Linux bench host."""
    if memory_mib <= 0:
        return None
    limit = memory_mib * 1024 * 1024

    def apply_limit() -> None:
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))

    return apply_limit


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--collection", type=Path, required=True)
    parser.add_argument("--analyzer", type=Path, required=True)
    parser.add_argument("--source-template",
                        help="fallback source name with {name}; the collection manifest is authoritative")
    parser.add_argument("--classic", action="store_true")
    parser.add_argument("--json-out", type=Path, required=True)
    parser.add_argument("--reports-dir", type=Path)
    parser.add_argument("--resume", action="store_true",
                        help="reuse successful rows from a matching checkpoint")
    parser.add_argument("--analyzer-memory-mib", type=int, default=2048,
                        help="child address-space cap; 0 disables (default: 2048)")
    args = parser.parse_args()
    bridge = load_bridge(root)
    source_names = source_names_from_manifest(args.collection)
    profiles = sorted(args.collection.glob("*.opt.profile.jsonl"))
    if not profiles:
        parser.error("collection has no opt profiles")
    reports_dir = args.reports_dir or args.json_out.parent / "region-reports"
    reports_dir.mkdir(parents=True, exist_ok=True)
    completed_rows: dict[str, dict] = {}
    if args.resume and args.json_out.is_file():
        prior = json.loads(args.json_out.read_text(encoding="utf-8"))
        if (prior.get("schema") != "capsid-profile-region-portfolio-v1" or
                prior.get("collection") != str(args.collection.resolve()) or
                prior.get("classic") != args.classic):
            parser.error("resume checkpoint does not match this collection/mode")
        completed_rows = {
            row["program"]: row for row in prior.get("programs", [])
            if "error" not in row
        }
    rows: list[dict] = []
    for profile in profiles:
        name = profile.name.removesuffix(".opt.profile.jsonl")
        if name in completed_rows:
            rows.append(completed_rows[name])
            continue
        bundle = args.collection / "bytecode" / f"{name}.rewrite.qjsb"
        if not bundle.is_file():
            bundle = args.collection / f"{name}.rewrite.qjsb"
        source_name = source_names.get(name)
        if source_name is None and args.source_template:
            source_name = args.source_template.format(name=name)
        if source_name is None:
            rows.append({"program": name,
                         "error": "source name absent from manifest and no fallback template"})
            write_output(args.json_out,
                         build_output(args.collection, args.classic, rows))
            continue
        if not bundle.is_file():
            rows.append({"program": name, "error": f"missing bundle: {bundle}"})
            write_output(args.json_out,
                         build_output(args.collection, args.classic, rows))
            continue
        try:
            sites, summary = bridge.load_sites(profile, source_name)
        except ValueError as error:
            rows.append({"program": name, "error": str(error)})
            write_output(args.json_out,
                         build_output(args.collection, args.classic, rows))
            continue
        with tempfile.TemporaryDirectory(prefix="capsid-regions-") as temp:
            tsv = Path(temp) / "sites.tsv"
            bridge.write_tsv(tsv, sites)
            mode = ("--regions-profile-classic" if args.classic
                    else "--regions-profile")
            completed = subprocess.run(
                [str(args.analyzer), mode, str(tsv), str(bundle)],
                check=False, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                preexec_fn=analyzer_limit(args.analyzer_memory_mib),
            )
        report_text = completed.stderr + completed.stdout
        (reports_dir / f"{name}.txt").write_text(report_text, encoding="utf-8")
        if completed.returncode != 0:
            rows.append({"program": name, "returncode": completed.returncode,
                         "error": report_text.strip()})
            write_output(args.json_out,
                         build_output(args.collection, args.classic, rows))
            continue
        try:
            parsed = parse_report(report_text)
        except ValueError as error:
            rows.append({"program": name, "error": str(error)})
            write_output(args.json_out,
                         build_output(args.collection, args.classic, rows))
            continue
        row = {"program": name, "profile": summary, **parsed}
        rows.append(row)
        write_output(args.json_out,
                     build_output(args.collection, args.classic, rows))

    output = build_output(args.collection, args.classic, rows)
    write_output(args.json_out, output)
    print(json.dumps({"programs": len(rows), "failures": output["failures"],
                      "aggregate": output["aggregate"]}, indent=2))
    return 1 if output["failures"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

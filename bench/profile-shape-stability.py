#!/usr/bin/env python3
"""Measure exact-site get_field shape stability from opcode-profile v4.

The profiler records two exact monotonic shape identities per site and an
aggregate count for further identities. This report deliberately treats 3+
shape sites as megamorphic: the first-two counts are not a top-k estimate.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


SCHEMA = "capsid-profile-shape-stability-v1"


def source_map(region_paths: list[Path]) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in region_paths:
        obj = json.loads(path.read_text(encoding="utf-8"))
        if obj.get("schema") != "capsid-profile-region-portfolio-v1":
            raise ValueError(f"{path}: unsupported regions schema")
        for program in obj.get("programs", []):
            name = str(program["program"])
            value = str(program.get("profile", {}).get("source_hash", "")).lower()
            if len(value) != 16:
                raise ValueError(f"{path}: invalid source hash for {name}")
            if name in result and result[name] != value:
                raise ValueError(f"conflicting source hashes for {name}")
            result[name] = value
    return result


def analyze_profile(label: str, path: Path, expected_source: str) -> dict:
    sites = 0
    selected_opcode_executions = 0
    get_field_executions = 0
    direct_observations = 0
    mono_observations = 0
    poly2_observations = 0
    megamorphic_observations = 0
    mono_sites = 0
    poly2_sites = 0
    megamorphic_sites = 0
    top_direct = 0
    runtimes = 0
    with path.open(encoding="utf-8", errors="strict") as stream:
        for line_number, raw in enumerate(stream, 1):
            if not raw.strip():
                continue
            obj = json.loads(raw)
            if obj.get("schema") != "quickjs-ng-opcode-profile-v4":
                raise ValueError(f"{path}:{line_number}: requires profile v4")
            if int(obj.get("site_overflow", 0)) != 0:
                raise ValueError(f"{path}:{line_number}: site overflow")
            runtimes += 1
            for site in obj.get("sites", []):
                if str(site.get("source_hash", "")).lower() != expected_source:
                    continue
                executions = int(site.get("exec", 0))
                selected_opcode_executions += executions
                if site.get("op") != "get_field":
                    continue
                get_field_executions += executions
                shape = site.get("shape", {})
                first = int(shape.get("first", 0))
                second = int(shape.get("second", 0))
                other = int(shape.get("other", 0))
                direct = first + second + other
                if direct == 0:
                    continue
                sites += 1
                direct_observations += direct
                top_direct = max(top_direct, direct)
                if other > 0:
                    megamorphic_sites += 1
                    megamorphic_observations += direct
                elif second > 0:
                    poly2_sites += 1
                    poly2_observations += direct
                else:
                    mono_sites += 1
                    mono_observations += direct
    if runtimes == 0:
        raise ValueError(f"{path}: no runtime profiles")
    stable = mono_observations + poly2_observations
    return {
        "program": label,
        "profile": str(path.resolve()),
        "runtimes": runtimes,
        "sites": sites,
        "mono_sites": mono_sites,
        "poly2_sites": poly2_sites,
        "megamorphic_sites": megamorphic_sites,
        "selected_opcode_executions": selected_opcode_executions,
        "get_field_executions": get_field_executions,
        "direct_observations": direct_observations,
        "mono_observations": mono_observations,
        "poly2_observations": poly2_observations,
        "megamorphic_observations": megamorphic_observations,
        "mono_share": mono_observations / direct_observations if direct_observations else 0,
        "mono_or_poly2_share": stable / direct_observations if direct_observations else 0,
        "direct_get_field_share": (
            direct_observations / get_field_executions if get_field_executions else 0),
        "direct_instruction_share": (
            direct_observations / selected_opcode_executions
            if selected_opcode_executions else 0),
        "top_site_concentration": (
            top_direct / direct_observations if direct_observations else 0),
    }


def summarize(programs: list[dict]) -> dict:
    keys = ("sites", "mono_sites", "poly2_sites", "megamorphic_sites",
            "selected_opcode_executions", "get_field_executions",
            "direct_observations", "mono_observations",
            "poly2_observations", "megamorphic_observations")
    aggregate = {key: sum(int(row[key]) for row in programs) for key in keys}
    direct = aggregate["direct_observations"]
    stable = aggregate["mono_observations"] + aggregate["poly2_observations"]
    aggregate["mono_share"] = aggregate["mono_observations"] / direct if direct else 0
    aggregate["mono_or_poly2_share"] = stable / direct if direct else 0
    aggregate["direct_instruction_share"] = (
        direct / aggregate["selected_opcode_executions"]
        if aggregate["selected_opcode_executions"] else 0)
    return {"schema": SCHEMA, "programs": programs, "aggregate": aggregate}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--regions", action="append", type=Path, required=True)
    parser.add_argument("--profile", action="append", required=True,
                        help="PROGRAM=profile.jsonl (repeatable)")
    parser.add_argument("--json-out", type=Path, required=True)
    args = parser.parse_args()
    sources = source_map(args.regions)
    programs = []
    for value in args.profile:
        if "=" not in value:
            parser.error("--profile must be PROGRAM=PATH")
        label, raw_path = value.split("=", 1)
        if label not in sources:
            parser.error(f"no source hash for {label}")
        programs.append(analyze_profile(label, Path(raw_path), sources[label]))
    output = summarize(programs)
    args.json_out.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    for row in programs:
        print(f"{row['program']}: direct={row['direct_observations']} "
              f"mono={row['mono_share']:.2%} "
              f"mono+poly2={row['mono_or_poly2_share']:.2%} "
              f"instruction-share={row['direct_instruction_share']:.2%} "
              f"top-site={row['top_site_concentration']:.2%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

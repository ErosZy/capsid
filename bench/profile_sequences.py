#!/usr/bin/env python3
"""Rank dynamically hot, physically adjacent QuickJS opcode sequences.

The opcode-profile v3 dump records every executed bytecode site as
``(source hash, runtime-local function id, exact PC, opcode, count)``. This tool joins
those sites with the canonical opcode sizes and enumerates adjacent windows of
2..8 instructions.  A window's conservative execution weight is the minimum
count of its members.  It is evidence for choosing a CFG+SSA fusion template,
not permission to lower the concrete runtime-local site: CFG/effect/ownership
proof still decides whether a static occurrence is legal.

No timing from the profiling build is consumed. The avoided-dispatch estimate
assumes a candidate window could become one directly dispatched operation;
CFG/effect/ownership proof and a product A/B still decide whether that vehicle
is worthwhile. Property-path counters are diagnostics and are not converted
into speculative savings.
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import pathlib
import re
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Mapping, MutableMapping, Sequence, Tuple


OPCODE_RE = re.compile(
    r"^\s*DEF\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([0-9]+)\s*,")

# Calls, control transfers, suspension and exception-region machinery are hard
# sequence boundaries.  Heap writes are also excluded from the first fusion
# experiment: a later guard failure must never need to roll one back.
BOUNDARY_EXACT = {
    "invalid", "eval", "apply_eval", "import", "throw", "throw_error",
    "catch", "gosub", "ret", "return", "return_undef", "return_async",
    "await", "yield", "yield_star", "async_yield_star", "iterator_call",
    "iterator_next", "iterator_close", "using_dispose",
    "using_dispose_async", "put_field", "put_array_el", "put_super_value",
    "define_field", "define_array_el", "copy_data_properties",
}
BOUNDARY_PREFIXES = (
    "call", "tail_call", "goto", "if_false", "if_true", "with_",
    "scope_", "throw_", "define_class", "define_method",
)

# Every retained sequence must contain work beyond load/store/stack traffic.
# These are still only census anchors; the eventual handler must demonstrate
# that it removes repeated checks/materialization rather than wrap one opcode.
HEAVY_EXACT = {
    "get_field", "get_field2", "get_array_el", "get_array_el2",
    "get_private_field", "get_super", "get_super_value", "get_length",
    "add", "sub", "mul", "div", "mod", "pow", "and", "or", "xor",
    "shl", "sar", "shr", "lt", "lte", "gt", "gte", "eq", "neq",
    "strict_eq", "strict_neq", "instanceof", "in", "typeof", "neg",
    "plus", "inc", "dec", "not", "lnot",
}

PROPERTY_SLOW_CLASSES = {
    "accessor_or_generic", "primitive_or_nullish", "missing_or_key_fallback",
}


def is_boundary(op: str) -> bool:
    return op in BOUNDARY_EXACT or op.startswith(BOUNDARY_PREFIXES)


def is_heavy(op: str) -> bool:
    return op in HEAVY_EXACT


def load_opcode_sizes(path: pathlib.Path) -> Dict[str, int]:
    sizes: Dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = OPCODE_RE.match(line)
        if match:
            sizes[match.group(1)] = int(match.group(2))
    if not sizes:
        raise ValueError(f"no DEF opcode rows found in {path}")
    return sizes


def load_profiles(path: pathlib.Path, mode: str) -> Iterable[Tuple[str, dict]]:
    for name in sorted(glob.glob(str(path / "*.profile.jsonl"))):
        file_mode = "source" if ".source." in name else "opt"
        if mode != "all" and file_mode != mode:
            continue
        with open(name, encoding="utf-8", errors="replace") as stream:
            for line in stream:
                line = line.strip()
                if not line.startswith("{"):
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("schema") not in (
                        "quickjs-ng-opcode-profile-v2",
                        "quickjs-ng-opcode-profile-v3"):
                    continue
                yield os.path.basename(name), obj


def source_hash(name: str) -> str:
    """QuickJS profile v3's 64-bit FNV-1a source identity."""
    value = 14695981039346656037
    for byte in name.encode("utf-8")[:255]:
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


@dataclass
class Aggregate:
    pattern: Tuple[str, ...]
    occurrences: int = 0
    region_exec: int = 0
    instruction_exec: int = 0
    avoided_dispatches: int = 0
    byte_exec: int = 0
    property_direct: int = 0
    property_slow: int = 0
    evidence: List[dict] = field(default_factory=list)
    program_files: set[str] = field(default_factory=set)

    def add(self, weight: int, byte_size: int, direct: int, slow: int,
            coordinate: dict) -> None:
        self.occurrences += 1
        self.region_exec += weight
        self.instruction_exec += weight * len(self.pattern)
        self.avoided_dispatches += weight * (len(self.pattern) - 1)
        self.byte_exec += weight * byte_size
        self.property_direct += direct
        self.property_slow += slow
        self.program_files.add(str(coordinate["file"]))
        self.evidence.append(coordinate)
        self.evidence.sort(key=lambda row: row["executions"], reverse=True)
        del self.evidence[5:]

    def as_dict(self) -> dict:
        return {
            "pattern": list(self.pattern),
            "length": len(self.pattern),
            "occurrences": self.occurrences,
            "region_exec": self.region_exec,
            "instruction_exec": self.instruction_exec,
            "avoided_dispatches": self.avoided_dispatches,
            "byte_exec": self.byte_exec,
            "property_direct": self.property_direct,
            "property_slow": self.property_slow,
            "program_count": len(self.program_files),
            "program_files": sorted(self.program_files),
            "evidence": self.evidence,
        }


def property_counts(sites: Sequence[Mapping[str, object]]) -> Tuple[int, int]:
    direct = 0
    slow = 0
    for site in sites:
        classes = site.get("prop", {})
        if not isinstance(classes, Mapping):
            continue
        direct += int(classes.get("direct", 0))
        for name in PROPERTY_SLOW_CLASSES:
            slow += int(classes.get(name, 0))
        # Field prototype data remains in QuickJS's inline prototype walk;
        # array prototype/int fallback enters the generic helper.
        if str(site.get("op", "")) in ("get_array_el", "get_array_el2"):
            slow += int(classes.get("prototype_or_int_fallback", 0))
    return direct, slow


def census(profiles: Iterable[Tuple[str, dict]], sizes: Mapping[str, int],
           min_length: int = 2, max_length: int = 8,
           min_exec: int = 1, source_name: str | None = None,
           source_template: str | None = None,
           min_programs: int = 1) -> dict:
    if min_length < 2 or max_length < min_length or max_length > 8:
        raise ValueError("sequence lengths must satisfy 2 <= min <= max <= 8")

    aggregates: MutableMapping[Tuple[str, ...], Aggregate] = {}
    stats = collections.Counter()
    for evidence_file, obj in profiles:
        stats["runtimes"] += 1
        stats["site_overflow"] += int(obj.get("site_overflow", 0))
        functions: MutableMapping[int, Dict[int, dict]] = collections.defaultdict(dict)
        expected_source = source_name
        if source_template is not None:
            fixture = evidence_file.split(".source.", 1)[0].split(".opt.", 1)[0]
            expected_source = source_template.format(name=fixture)
        expected_hash = source_hash(expected_source) if expected_source else None
        for raw in obj.get("sites", []):
            if expected_hash is not None:
                actual_hash = raw.get("source_hash")
                if actual_hash is None:
                    stats["missing_source_hash"] += 1
                    continue
                if str(actual_hash).lower() != expected_hash:
                    stats["foreign_source_sites"] += 1
                    continue
            try:
                function = int(raw["function"])
                pc = int(raw["pc"])
                executions = int(raw["exec"])
                op = str(raw["op"])
            except (KeyError, TypeError, ValueError):
                stats["malformed_sites"] += 1
                continue
            if pc in functions[function]:
                stats["duplicate_sites"] += 1
                continue
            functions[function][pc] = {
                **raw, "function": function, "pc": pc,
                "exec": executions, "op": op,
            }
            stats["sites"] += 1

        stats["functions"] += len(functions)
        for function, by_pc in functions.items():
            for start_pc in sorted(by_pc):
                stats["starts"] += 1
                window: List[dict] = []
                pc = start_pc
                for length in range(1, max_length + 1):
                    site = by_pc.get(pc)
                    if site is None:
                        stats["missing_successor"] += 1
                        break
                    op = str(site["op"])
                    size = sizes.get(op)
                    if size is None:
                        stats["unknown_opcode"] += 1
                        break
                    if is_boundary(op):
                        stats["control_or_effect_boundary"] += 1
                        break
                    window.append(site)
                    pc += size
                    if length < min_length:
                        continue
                    if not any(is_heavy(str(member["op"])) for member in window):
                        stats["no_heavy_member"] += 1
                        continue
                    weight = min(int(member["exec"]) for member in window)
                    if weight < min_exec:
                        stats["below_min_exec"] += 1
                        continue
                    pattern = tuple(str(member["op"]) for member in window)
                    direct, slow = property_counts(window)
                    byte_size = sum(sizes[name] for name in pattern)
                    coordinate = {
                        "file": evidence_file,
                        "runtime": int(obj.get("runtime", 0)),
                        # Runtime-local coordinate: the pattern, not this id,
                        # is what the static CFG census is allowed to consume.
                        "function": function,
                        "pc": start_pc,
                        "executions": weight,
                    }
                    aggregate = aggregates.setdefault(pattern, Aggregate(pattern))
                    aggregate.add(weight, byte_size, direct, slow, coordinate)
                    stats["candidate_windows"] += 1

    rows = sorted((row for row in aggregates.values()
                   if len(row.program_files) >= min_programs),
                  key=lambda row: (row.avoided_dispatches,
                                   row.instruction_exec,
                                   len(row.pattern), row.pattern),
                  reverse=True)
    return {
        "schema": "capsid-opcode-sequence-census-v2",
        "identity": "source-hash+profile-file+runtime+runtime-local-function+pc",
        "selection_key": "opcode-pattern; static CFG+SSA must re-prove sites",
        "stats": dict(sorted(stats.items())),
        "sequences": [row.as_dict() for row in rows],
    }


def print_report(report: Mapping[str, object], top: int) -> None:
    stats = report["stats"]
    assert isinstance(stats, Mapping)
    print("== exact-PC adjacent sequence census ==")
    print(" ".join(f"{key}={value}" for key, value in stats.items()))
    print("rank dispatch_saved region_exec programs occurrences len pattern")
    sequences = report["sequences"]
    assert isinstance(sequences, Sequence)
    for rank, raw in enumerate(sequences[:top], 1):
        assert isinstance(raw, Mapping)
        pattern = " > ".join(str(op) for op in raw["pattern"])
        print(f"{rank:>4} {int(raw['avoided_dispatches']):>14,} "
              f"{int(raw['region_exec']):>11,} {int(raw['program_count']):>8,} "
              f"{int(raw['occurrences']):>11,} "
              f"{int(raw['length']):>3} {pattern}")
        evidence = raw.get("evidence", [])
        if evidence:
            first = evidence[0]
            print("     top_site="
                  f"{first['file']}:r{first['runtime']}:"
                  f"f{first['function']}:pc{first['pc']} "
                  f"exec={int(first['executions']):,} "
                  f"prop_direct={int(raw['property_direct']):,} "
                  f"prop_slow={int(raw['property_slow']):,}")


def main() -> None:
    root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("dir", type=pathlib.Path,
                        help="profile collection directory")
    parser.add_argument("--mode", choices=("source", "opt", "all"),
                        default="opt")
    parser.add_argument("--opcode-header", type=pathlib.Path,
                        default=root / "vendor/txiki.js/deps/quickjs/quickjs-opcode.h")
    parser.add_argument("--min-length", type=int, default=2)
    parser.add_argument("--max-length", type=int, default=8)
    parser.add_argument("--min-exec", type=int, default=1)
    parser.add_argument(
        "--min-programs", type=int, default=1,
        help="keep patterns observed in at least this many profile files")
    source_group = parser.add_mutually_exclusive_group()
    source_group.add_argument(
        "--source-name",
        help="keep only v3 sites whose bytecode filename matches this source")
    source_group.add_argument(
        "--source-template",
        help="per-file source template; {name} is the profile filename stem")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--json-out", type=pathlib.Path)
    args = parser.parse_args()

    report = census(load_profiles(args.dir, args.mode),
                    load_opcode_sizes(args.opcode_header),
                    args.min_length, args.max_length, args.min_exec,
                    args.source_name, args.source_template,
                    args.min_programs)
    print_report(report, args.top)
    if args.json_out:
        args.json_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                 encoding="utf-8")


if __name__ == "__main__":
    main()

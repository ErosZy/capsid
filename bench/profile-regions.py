#!/usr/bin/env python3
"""Join opcode-profile v4 sites to the CFG+SSA region census.

This is the only supported dynamic-to-static bridge. It filters bootstrap and
foreign-source sites, rejects incomplete tables, converts stable function
identities to a small TSV contract, and invokes the analyze-only census on the
exact deployed bytecode bundle that produced the profile.
"""

from __future__ import annotations

import argparse
import collections
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def source_hash(name: str) -> str:
    value = 14695981039346656037
    for byte in name.encode("utf-8")[:255]:
        value ^= byte
        value = (value * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def load_sites(profile_path: Path, source_name: str) -> tuple[list[tuple], dict]:
    expected_source = source_hash(source_name)
    counts: collections.Counter[tuple] = collections.Counter()
    runtimes = 0
    foreign = 0
    raw_sites = 0
    with profile_path.open(encoding="utf-8", errors="strict") as stream:
        for line_number, raw in enumerate(stream, 1):
            line = raw.strip()
            if not line:
                continue
            try:
                profile = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(
                    f"invalid profile JSON on line {line_number}: {error}") from error
            if profile.get("schema") != "quickjs-ng-opcode-profile-v4":
                raise ValueError("profile-regions requires opcode profile v4")
            overflow = int(profile.get("site_overflow", 0))
            if overflow != 0:
                raise ValueError(f"incomplete exact-site table: overflow={overflow}")
            runtimes += 1
            for site in profile.get("sites", []):
                raw_sites += 1
                if str(site.get("source_hash", "")).lower() != expected_source:
                    foreign += 1
                    continue
                try:
                    code_hash = str(site["code_hash"]).lower()
                    if len(code_hash) != 16:
                        raise ValueError("code_hash is not 16 hex digits")
                    int(code_hash, 16)
                    key = (
                        code_hash,
                        int(site["code_len"]),
                        int(site["line"]),
                        int(site["column"]),
                        int(site["pc"]),
                    )
                    executions = int(site["exec"])
                except (KeyError, TypeError, ValueError) as error:
                    raise ValueError(
                        f"malformed stable site on line {line_number}") from error
                if min(*key[1:], executions) < 0:
                    raise ValueError("negative stable site field")
                counts[key] += executions
    if runtimes == 0:
        raise ValueError("profile contains no v4 runtime dumps")
    if not counts:
        raise ValueError("profile contains no sites for the requested source")
    rows = [(*key, executions) for key, executions in sorted(counts.items())]
    return rows, {
        "runtimes": runtimes,
        "raw_sites": raw_sites,
        "foreign_sites": foreign,
        "selected_sites": len(rows),
        "source_hash": expected_source,
    }


def write_tsv(path: Path, rows: list[tuple]) -> None:
    path.write_text(
        "# code_hash code_len line column pc executions\n" +
        "".join(" ".join(str(value) for value in row) + "\n" for row in rows),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--source-name", required=True)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--analyzer", type=Path, required=True)
    parser.add_argument("--tsv-out", type=Path)
    parser.add_argument("--classic", action="store_true",
                        help="analyze a top-level global script, not a module")
    args = parser.parse_args()
    try:
        rows, summary = load_sites(args.profile, args.source_name)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(summary, sort_keys=True))

    if args.tsv_out:
        write_tsv(args.tsv_out, rows)
        tsv_path = args.tsv_out
        analyzer_mode = ("--regions-profile-classic" if args.classic
                         else "--regions-profile")
        return subprocess.run(
            [str(args.analyzer), analyzer_mode, str(tsv_path),
             str(args.bundle)], check=False,
        ).returncode

    with tempfile.TemporaryDirectory(prefix="capsid-profile-regions-") as temp:
        tsv_path = Path(temp) / "sites.tsv"
        write_tsv(tsv_path, rows)
        analyzer_mode = ("--regions-profile-classic" if args.classic
                         else "--regions-profile")
        return subprocess.run(
            [str(args.analyzer), analyzer_mode, str(tsv_path),
             str(args.bundle)], check=False,
        ).returncode


if __name__ == "__main__":
    raise SystemExit(main())

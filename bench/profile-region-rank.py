#!/usr/bin/env python3
"""Cross-portfolio breadth/concentration report for CFG+SSA regions.

Raw execution totals from time-budgeted V8, classic suites, microfixtures and
framework requests are not commensurate. This tool therefore ranks breadth
first and reports concentration instead of pretending their counts can be
summed into a universal speedup score.
"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


SCHEMA = "capsid-profile-region-cross-portfolio-v1"


def selected_opcode_executions(profile_path: Path, source_hash: str) -> int:
    """Return executed opcodes belonging to the selected source only.

    Whole-runtime totals include bootstrap/worker functions and would
    systematically understate AOT coverage. The v4 exact-site table is
    complete when site_overflow is zero, so it is the authoritative
    denominator for a program-normalized instruction share.
    """
    total = 0
    runtimes = 0
    with profile_path.open(encoding="utf-8", errors="strict") as stream:
        for line_number, raw in enumerate(stream, 1):
            if not raw.strip():
                continue
            obj = json.loads(raw)
            if obj.get("schema") != "quickjs-ng-opcode-profile-v4":
                raise ValueError(f"{profile_path}:{line_number}: requires v4")
            if int(obj.get("site_overflow", 0)) != 0:
                raise ValueError(f"{profile_path}:{line_number}: site overflow")
            runtimes += 1
            for site in obj.get("sites", []):
                if str(site.get("source_hash", "")).lower() == source_hash:
                    total += int(site.get("exec", 0))
    if runtimes == 0 or total <= 0:
        raise ValueError(f"{profile_path}: no selected-source executions")
    return total


def load_portfolio(label: str, path: Path) -> dict:
    obj = json.loads(path.read_text(encoding="utf-8"))
    if obj.get("schema") != "capsid-profile-region-portfolio-v1":
        raise ValueError(f"{path}: unsupported schema")
    if obj.get("failures") != 0:
        raise ValueError(f"{path}: contains {obj.get('failures')} failures")
    collection = Path(obj["collection"])
    for program in obj.get("programs", []):
        profile_path = collection / f"{program['program']}.opt.profile.jsonl"
        source_hash = str(program.get("profile", {}).get("source_hash", ""))
        if len(source_hash) != 16:
            raise ValueError(f"{path}: missing source hash for {program['program']}")
        program["selected_opcode_executions"] = selected_opcode_executions(
            profile_path, source_hash.lower())
    return {"label": label, "path": str(path.resolve()), "report": obj}


def rank(portfolios: list[dict]) -> dict:
    template_names = sorted({
        name
        for portfolio in portfolios
        for name in portfolio["report"].get("aggregate", {})
    })
    rows = []
    for template in template_names:
        corpus_rows = []
        hot_programs_total = 0
        dynamic_tiebreak = 0
        for portfolio in portfolios:
            report = portfolio["report"]
            programs = report.get("programs", [])
            samples = []
            instruction_shares = []
            selected_exec = 0
            matched_insns = 0
            for program in programs:
                counters = program.get("templates", {}).get(template, {})
                dynamic = int(counters.get("dynamic_regions", 0))
                predicted = int(counters.get("dynamic_predicted", 0))
                dynamic_insns = int(counters.get("dynamic_insns", 0))
                if dynamic > 0:
                    denominator = int(program.get(
                        "selected_opcode_executions", 0))
                    share = ((dynamic_insns / denominator)
                             if denominator > 0 else 0.0)
                    samples.append((program["program"], dynamic, predicted,
                                    share))
                    instruction_shares.append(share)
                    selected_exec += denominator
                    matched_insns += dynamic_insns
            aggregate = report.get("aggregate", {}).get(template, {})
            total_dynamic = int(aggregate.get("dynamic_regions", 0))
            top = max(samples, key=lambda item: item[1]) if samples else None
            concentration = ((top[1] / total_dynamic) if top and total_dynamic
                             else 0.0)
            hot_programs_total += len(samples)
            dynamic_tiebreak += int(aggregate.get("dynamic_predicted", 0))
            corpus_rows.append({
                "portfolio": portfolio["label"],
                "programs": len(programs),
                "hot_programs": len(samples),
                "dynamic_regions": total_dynamic,
                "dynamic_predicted": int(
                    aggregate.get("dynamic_predicted", 0)),
                "top_program": top[0] if top else None,
                "top_dynamic_regions": top[1] if top else 0,
                "top_concentration": concentration,
                "weighted_instruction_share": (
                    matched_insns / selected_exec if selected_exec else 0.0),
                "median_hot_program_instruction_share": (
                    statistics.median(instruction_shares)
                    if instruction_shares else 0.0),
                "max_hot_program_instruction_share": (
                    max(instruction_shares) if instruction_shares else 0.0),
            })
        hot_portfolios = sum(row["hot_programs"] > 0 for row in corpus_rows)
        rows.append({
            "template": template,
            "hot_portfolios": hot_portfolios,
            "portfolio_count": len(portfolios),
            "hot_programs": hot_programs_total,
            "dynamic_tiebreak": dynamic_tiebreak,
            "portfolios": corpus_rows,
        })
    rows.sort(key=lambda row: (-row["hot_portfolios"],
                              -row["hot_programs"],
                              -row["dynamic_tiebreak"], row["template"]))
    return {
        "schema": SCHEMA,
        "ranking_policy": [
            "hot_portfolios descending",
            "hot_programs descending",
            "dynamic_predicted sum only as a non-comparable tiebreak",
        ],
        "inputs": [{"label": p["label"], "path": p["path"]}
                   for p in portfolios],
        "templates": rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--portfolio", action="append", required=True,
                        help="LABEL=regions.json (repeatable)")
    parser.add_argument("--json-out", type=Path, required=True)
    args = parser.parse_args()
    portfolios = []
    for value in args.portfolio:
        if "=" not in value:
            parser.error("--portfolio must be LABEL=PATH")
        label, raw_path = value.split("=", 1)
        portfolios.append(load_portfolio(label, Path(raw_path)))
    output = rank(portfolios)
    args.json_out.write_text(json.dumps(output, indent=2) + "\n",
                             encoding="utf-8")
    print("rank portfolios hot-programs template")
    for index, row in enumerate(output["templates"], 1):
        print(f"{index:4d} {row['hot_portfolios']:10d} "
              f"{row['hot_programs']:12d} {row['template']}")
        for corpus in row["portfolios"]:
            if not corpus["hot_programs"]:
                continue
            print(f"     {corpus['portfolio']}: "
                  f"{corpus['hot_programs']}/{corpus['programs']} programs, "
                  f"dynamic {corpus['dynamic_regions']}, "
                  f"top {corpus['top_program']} "
                  f"{corpus['top_concentration']:.1%}, "
                  f"instruction share weighted/median/max "
                  f"{corpus['weighted_instruction_share']:.3%}/"
                  f"{corpus['median_hot_program_instruction_share']:.3%}/"
                  f"{corpus['max_hot_program_instruction_share']:.3%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

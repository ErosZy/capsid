#!/usr/bin/env python3
"""Directed breadth/concentration test for profile-region-rank.py."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    path = root / "bench/profile-region-rank.py"
    spec = importlib.util.spec_from_file_location("capsid_region_rank", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    a = {
        "label": "a", "path": "/a", "report": {
            "programs": [
                {"program": "a1", "selected_opcode_executions": 1000,
                 "templates": {
                    "broad": {"dynamic_regions": 90,
                              "dynamic_predicted": 90,
                              "dynamic_insns": 180}}},
                {"program": "a2", "selected_opcode_executions": 2000,
                 "templates": {
                    "broad": {"dynamic_regions": 10,
                              "dynamic_predicted": 10,
                              "dynamic_insns": 20},
                    "narrow": {"dynamic_regions": 1000,
                               "dynamic_predicted": 1000,
                               "dynamic_insns": 1000}}},
            ],
            "aggregate": {
                "broad": {"dynamic_regions": 100,
                          "dynamic_predicted": 100},
                "narrow": {"dynamic_regions": 1000,
                           "dynamic_predicted": 1000},
            },
        },
    }
    b = {
        "label": "b", "path": "/b", "report": {
            "programs": [{"program": "b1", "selected_opcode_executions": 100,
                          "templates": {
                "broad": {"dynamic_regions": 5,
                          "dynamic_predicted": 5,
                          "dynamic_insns": 10}}}],
            "aggregate": {"broad": {"dynamic_regions": 5,
                                     "dynamic_predicted": 5}},
        },
    }
    report = module.rank([a, b])
    assert report["schema"] == module.SCHEMA
    assert report["templates"][0]["template"] == "broad"
    assert report["templates"][0]["hot_portfolios"] == 2
    assert report["templates"][0]["hot_programs"] == 3
    assert report["templates"][0]["portfolios"][0][
        "top_concentration"] == 0.9
    assert report["templates"][0]["portfolios"][0][
        "weighted_instruction_share"] == 0.2 / 3
    assert report["templates"][0]["portfolios"][0][
        "median_hot_program_instruction_share"] == 0.095
    print("test_profile_region_rank: all green")


if __name__ == "__main__":
    main()

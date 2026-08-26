#!/usr/bin/env python3
"""Directed contract for profile-shape-stability.py."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    path = root / "bench/profile-shape-stability.py"
    spec = importlib.util.spec_from_file_location("capsid_shape_profile", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory() as temp:
        profile = Path(temp) / "p.jsonl"
        obj = {
            "schema": "quickjs-ng-opcode-profile-v4", "site_overflow": 0,
            "sites": [
                {"source_hash": "0123456789abcdef", "op": "get_field",
                 "exec": 10, "shape": {"first": 10, "second": 0, "other": 0}},
                {"source_hash": "0123456789abcdef", "op": "get_field",
                 "exec": 20, "shape": {"first": 12, "second": 8, "other": 0}},
                {"source_hash": "0123456789abcdef", "op": "get_field",
                 "exec": 30, "shape": {"first": 5, "second": 5, "other": 20}},
                {"source_hash": "0123456789abcdef", "op": "add", "exec": 40,
                 "shape": {"first": 0, "second": 0, "other": 0}},
                {"source_hash": "ffffffffffffffff", "op": "get_field",
                 "exec": 999, "shape": {"first": 999, "second": 0, "other": 0}},
            ],
        }
        profile.write_text(json.dumps(obj) + "\n", encoding="utf-8")
        row = module.analyze_profile("p", profile, "0123456789abcdef")
        assert row["sites"] == 3
        assert row["mono_sites"] == 1 and row["poly2_sites"] == 1
        assert row["megamorphic_sites"] == 1
        assert row["selected_opcode_executions"] == 100
        assert row["direct_observations"] == 60
        assert row["mono_share"] == 1 / 6
        assert row["mono_or_poly2_share"] == 0.5
        assert row["direct_instruction_share"] == 0.6
        report = module.summarize([row])
        assert report["schema"] == module.SCHEMA
        assert report["aggregate"]["mono_or_poly2_share"] == 0.5
    print("test_profile_shape_stability: all green")


if __name__ == "__main__":
    main()

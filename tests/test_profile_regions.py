#!/usr/bin/env python3
"""Directed contract for the stable profile-to-region bridge."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location("capsid_profile_regions", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    module = load_module(root / "bench/profile-regions.py")
    name = "file:///app.js"
    stable = {
        "function": 99,
        "source_hash": module.source_hash(name),
        "code_hash": "0123456789abcdef",
        "code_len": 42,
        "line": 7,
        "column": 3,
        "pc": 9,
        "exec": 11,
    }
    foreign = {**stable, "source_hash": module.source_hash("tjs:bootstrap")}
    profile = {
        "schema": "quickjs-ng-opcode-profile-v4",
        "sites": [stable, stable, foreign],
        "site_overflow": 0,
    }
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "profile.jsonl"
        path.write_text(json.dumps(profile) + "\n", encoding="utf-8")
        rows, summary = module.load_sites(path, name)
        assert rows == [("0123456789abcdef", 42, 7, 3, 9, 22)]
        assert summary["foreign_sites"] == 1

        profile["site_overflow"] = 1
        path.write_text(json.dumps(profile) + "\n", encoding="utf-8")
        try:
            module.load_sites(path, name)
            raise AssertionError("overflow profile was accepted")
        except ValueError as error:
            assert "overflow=1" in str(error)
    print("test_profile_regions: all green")


if __name__ == "__main__":
    main()

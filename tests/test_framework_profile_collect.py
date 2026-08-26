#!/usr/bin/env python3
"""Directed contracts for the four-framework opcode profile collector."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location(
        "capsid_framework_profile_collect", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    module = load_module(root / "bench/framework-profile-collect.py")
    profile = {
        "schema": "quickjs-ng-opcode-profile-v4",
        "runtime": 1,
        "sites": [],
    }
    lines = [
        "ordinary stderr\n",
        json.dumps(profile) + "\n",
        '{"schema":"unrelated"}\n',
        "{truncated\n",
    ]
    assert module.extract_profiles(lines) == [profile]
    assert module.source_hash(module.FRAMEWORK_SOURCE_NAME) == "04861a1d34c0d324"

    rows = module.framework_matrix(root, root / "build-profile")
    assert [row["name"] for row in rows] == [
        "hono", "elysia", "h3-v2", "itty-router",
    ]
    assert rows[-1]["extra"] == ["--variant", "autorouter"]
    print("test_framework_profile_collect: all green")


if __name__ == "__main__":
    main()

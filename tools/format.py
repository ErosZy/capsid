#!/usr/bin/env python3
"""Format maintained Git-listed sources, never vendor/generated/evidence payloads."""

import argparse
import fnmatch
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def classify(name, policy):
    """A shared allowlist, rather than recursively traversing dependency trees."""
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "node_modules" in path.parts:
        return None
    if (
        name in policy.get("preservedInputs", {})
        or name in policy["excludeFiles"]
        or any(name.startswith(prefix) for prefix in policy["excludePrefixes"])
    ):
        return None
    if name in policy["dataFiles"] or any(
        fnmatch.fnmatchcase(name, pattern) for pattern in policy["dataGlobs"]
    ):
        return "data"
    if not path.parts or path.parts[0] not in policy["sourceRoots"]:
        return None
    for language, suffixes in policy["extensions"].items():
        if path.suffix.lower() in suffixes:
            return language
    return None


def select_files(root, policy, requested=(), language="all"):
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        capture_output=True,
        check=True,
    )
    names = sorted(set(result.stdout.decode("utf-8").split("\0")) - {""})
    selected = {key: [] for key in [*policy["extensions"], "data"]}
    requested = set(requested)
    if requested - set(names):
        raise ValueError("requested files must be non-ignored Git-listed paths")
    for name in names:
        group = classify(name, policy)
        if requested and name not in requested:
            continue
        if group is None:
            if name in requested:
                raise ValueError(f"protected or unsupported formatting input: {name}")
            continue
        path = root / name
        if (
            path.is_symlink()
            or not path.is_file()
            or not path.resolve().is_relative_to(root.resolve())
        ):
            raise ValueError(f"unsafe formatting input: {name}")
        if language == "all" or group == language:
            selected[group].append(name)
    if not any(selected.values()):
        raise ValueError("format selection is empty")
    return selected


def executable(name, tool_dir):
    filename = name + (".exe" if os.name == "nt" else "")
    candidates = [tool_dir / filename, Path(sys.executable).parent / filename]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    raise ValueError(f"missing {name}; install tools/format-requirements.txt in .venv-format")


def batches(names, maximum=12000):
    batch, size = [], 0
    for name in names:
        if batch and size + len(name) + 3 > maximum:
            yield batch
            batch, size = [], 0
        batch.append(name)
        size += len(name) + 3
    if batch:
        yield batch


def run(argv):
    subprocess.run(argv, cwd=ROOT, check=True)


def verify_version(argv, expected):
    result = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, check=True)
    if not re.search(r"(?<![0-9.])" + re.escape(expected) + r"(?![0-9.])", result.stdout):
        raise ValueError(
            f"formatter version mismatch: expected {expected}: {result.stdout.strip()}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="apply formatting")
    mode.add_argument("--check", action="store_true", help="read-only check (default)")
    mode.add_argument("--list", action="store_true", help="show selected paths, without tools")
    parser.add_argument(
        "--language", choices=["all", "cpp", "javascript", "python", "data"], default="all"
    )
    parser.add_argument(
        "--tool-dir", type=Path, help="directory containing pinned Python tool executables"
    )
    parser.add_argument(
        "files", nargs="*", help="optional repo-relative paths; protected paths are errors"
    )
    args = parser.parse_args()
    policy = json.loads((ROOT / "tools/format-policy.json").read_text(encoding="utf-8"))
    selected = select_files(ROOT, policy, args.files, args.language)
    if args.list:
        print(json.dumps(selected, indent=2))
        return
    tool_dir = args.tool_dir or ROOT / ".venv-format" / ("Scripts" if os.name == "nt" else "bin")
    if selected["cpp"]:
        clang = executable("clang-format", tool_dir)
        verify_version([clang, "--version"], policy["versions"]["clang-format"])
        for names in batches(selected["cpp"]):
            run(
                [
                    clang,
                    "--style=file",
                    "--fallback-style=none",
                    *(["-i"] if args.write else ["--dry-run", "--Werror"]),
                    *names,
                ]
            )
    prettier_files = selected["javascript"] + selected["data"]
    if prettier_files:
        node = shutil.which("node")
        cli = ROOT / "node_modules/prettier/bin/prettier.cjs"
        if not node or not cli.is_file():
            raise ValueError("missing Node/Prettier; run npm ci --ignore-scripts")
        verify_version([node, str(cli), "--version"], policy["versions"]["prettier"])
        for names in batches(prettier_files):
            run(
                [
                    node,
                    str(ROOT / "tools/format-js.mjs"),
                    "--write" if args.write else "--check",
                    *names,
                ]
            )
    if selected["python"]:
        ruff = executable("ruff", tool_dir)
        verify_version([ruff, "--version"], policy["versions"]["ruff"])
        for names in batches(selected["python"]):
            run([ruff, "format", *([] if args.write else ["--check"]), *names])
    print(
        json.dumps(
            {
                "formatted" if args.write else "checked": {
                    key: len(value) for key, value in selected.items()
                }
            }
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"format failed: {error}", file=sys.stderr)
        raise SystemExit(1)

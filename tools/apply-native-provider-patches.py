#!/usr/bin/env python3
"""Apply the reviewed native-provider corrections to exact upstream sources."""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def read_json(file):
    return json.loads(file.read_text(encoding="utf-8"), object_pairs_hook=unique_object)


def relative_name(name):
    require(isinstance(name, str) and name, "nonempty relative file name required")
    path = PurePosixPath(name)
    require(not path.is_absolute() and path.as_posix() == name and
            all(p not in (".", "..", ".git") for p in path.parts) and
            ":" not in name and "\\" not in name and "\n" not in name and "\r" not in name,
            "unsafe relative file name: " + name)
    return name


def hashes(mapping):
    require(isinstance(mapping, dict) and mapping, "nonempty file manifest required")
    for name, value in mapping.items():
        relative_name(name)
        require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value),
                "invalid file digest: " + name)
    return mapping


def plain_file(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and not path.is_symlink() and
            not getattr(info, "st_file_attributes", 0) & 0x400,
            "regular non-reparse file required: " + str(path))


def tree_hashes(source):
    result = {}
    for parent, directories, files in os.walk(source, followlinks=False):
        parent = Path(parent)
        if parent == source and ".git" in directories:
            directories.remove(".git")
        for directory in directories:
            path = parent / directory
            info = path.lstat()
            require(not path.is_symlink() and not getattr(info, "st_file_attributes", 0) & 0x400,
                    "source directory cannot be a link or reparse point: " + str(path))
        for filename in files:
            path = parent / filename
            if parent == source and filename == ".git":
                continue
            plain_file(path)
            name = relative_name(path.relative_to(source).as_posix())
            result[name] = digest(path.read_bytes())
    return result


def verify_tree(source, expected):
    actual = tree_hashes(source)
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    changed = sorted(name for name in set(actual) & set(expected) if actual[name] != expected[name])
    require(not (missing or extra or changed), "source manifest mismatch: " + json.dumps(
        dict(missing=missing[:8], extra=extra[:8], changed=changed[:8])))


def adjacent_file(manifest_file, name):
    relative_name(name)
    require(len(PurePosixPath(name).parts) == 1, "provider input must be beside the manifest")
    file = manifest_file.parent / name
    plain_file(file)
    return file


def git_apply(source, patch, check=False):
    command = ["git", "-c", "core.autocrlf=false", "apply"]
    if check:
        command.append("--check")
    command.append(str(patch))
    result = subprocess.run(command, cwd=source, capture_output=True, timeout=60)
    require(result.returncode == 0, "git apply failed: " + result.stderr.decode("utf-8", errors="replace")[:2000])


def prepare(args):
    manifest_file = args.manifest.resolve(strict=True)
    lock = read_json(manifest_file)
    require(lock.get("schema") == "capsid-qualified-native-providers-v1", "unsupported provider manifest")
    for flag in ["upstreamAssertionsAndRecipeSelectionPreserved",
                 "locallyPreparedCorrectionsNotUpstreamBackports", "noSanitizerSuppressions"]:
        require(lock.get(flag) is True, "missing provider provenance: " + flag)
    entry = lock["providers"][args.provider]
    patch = adjacent_file(manifest_file, entry["patch"])
    files = adjacent_file(manifest_file, entry["files"])
    require(digest(patch.read_bytes()) == entry["patchSHA256"], "provider patch digest mismatch")
    require(digest(files.read_bytes()) == entry["filesSHA256"], "provider file-manifest digest mismatch")
    require(re.fullmatch(r"[0-9a-f]{64}", entry["source"]["sha256"]), "invalid upstream archive digest")
    document = read_json(files)
    require(document.get("schema") == "capsid-native-provider-files-v1", "unsupported provider file manifest")
    original, patched = hashes(document["original"]), hashes(document["patched"])
    require(set(original) <= set(patched), "provider corrections cannot remove upstream files")
    changed = sorted(name for name, value in original.items() if patched[name] != value)
    added = sorted(set(patched) - set(original))
    require(changed == entry["changedFiles"] and added == entry["addedFiles"], "provider changed-file inventory mismatch")
    require(len(original) == entry["originalFiles"] and len(patched) == entry["patchedFiles"], "provider file count mismatch")
    for name in changed + added:
        relative_name(name)
    text = patch.read_text(encoding="utf-8")
    before = [line[4:] for line in text.splitlines() if line.startswith("--- ")]
    after = [line[4:] for line in text.splitlines() if line.startswith("+++ ")]
    names = sorted(changed + added)
    require(before == ["a/" + n if n in original else "/dev/null" for n in names] and
            after == ["b/" + n for n in names], "patch targets differ from the reviewed inventory")
    require(not re.search(r"^(?:diff --git|(?:new|old) file mode|rename |copy |GIT binary patch)", text, re.M),
            "only reviewed regular-file unified patches are supported")
    source = args.source.resolve(strict=True)
    require(source.is_dir(), "provider source must be a directory")
    if args.verify_only:
        verify_tree(source, patched)
        status = "patched-source-verified"
    else:
        verify_tree(source, original)
        # Replay just the changed inputs before touching the actual source.
        # This also checks that patch contents and declared result hashes agree.
        temp_parent = Path(tempfile.gettempdir()).resolve()
        with tempfile.TemporaryDirectory(prefix="capsid-provider-verify-", dir=temp_parent) as temporary:
            replay = Path(temporary).resolve()
            require(replay.parent == temp_parent and replay.name.startswith("capsid-provider-verify-"),
                    "owned patch replay directory required")
            for name in changed:
                target = replay / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes((source / name).read_bytes())
            git_apply(replay, patch, check=True)
            git_apply(replay, patch)
            verify_tree(replay, {name: patched[name] for name in names})
        verify_tree(source, original)
        git_apply(source, patch, check=True)
        if args.dry_run:
            status = "original-source-and-patch-verified"
        else:
            git_apply(source, patch)
            verify_tree(source, patched)
            status = "corrections-applied"
    return dict(schema="capsid-native-provider-preparation-v1", provider=args.provider,
                version=entry["version"], status=status, source=str(source),
                sourceFiles=len(patched) if not args.dry_run else len(original),
                upstreamArchiveSHA256=entry["source"]["sha256"], patchSHA256=entry["patchSHA256"],
                changedFiles=len(changed), addedFiles=len(added))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--provider", required=True, choices=["openssl", "cares"])
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--manifest", type=Path, default=ROOT / "patches/native-providers/manifest.json")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify-only", action="store_true")
    mode.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(prepare(args), sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print("FAIL native provider preparation: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

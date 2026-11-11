#!/usr/bin/env python3
"""Exercise the real provider tool on owned, synthetic source trees."""
from pathlib import Path
import difflib
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/apply-native-provider-patches.py"
sha = lambda data: hashlib.sha256(data).hexdigest()


class ProviderPatchTests(unittest.TestCase):
    def setUp(self):
        parent = Path(tempfile.gettempdir()).resolve()
        self.temporary = tempfile.TemporaryDirectory(prefix="capsid-provider-controls-", dir=parent)
        self.directory = Path(self.temporary.name).resolve()
        self.assertEqual(self.directory.parent, parent)
        self.assertTrue(self.directory.name.startswith("capsid-provider-controls-"))
        self.addCleanup(self.temporary.cleanup)
        self.source = self.directory / "source"
        self.source.mkdir()
        self.original = {"lib/callback.c": b"int callback(void *value) { return value != 0; }\n",
                         "test/original.c": b"assert(callback(0) == 0);\n"}
        self.patched = {**self.original,
                        "lib/callback.c": b"int callback(const void *value) { return value != 0; }\n",
                        "include/typed.h": b"int callback(const void *value);\n"}
        for name, data in self.original.items():
            file = self.source / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_bytes(data)
        self.patch = self.directory / "correction.patch"
        self.files = self.directory / "files.json"
        self.manifest = self.directory / "manifest.json"
        patch = []
        for name in sorted(self.patched):
            if self.original.get(name) == self.patched[name]:
                continue
            patch.extend(difflib.unified_diff(self.original.get(name, b"").decode().splitlines(True),
                self.patched[name].decode().splitlines(True),
                fromfile="a/" + name if name in self.original else "/dev/null", tofile="b/" + name))
        self.patch.write_text("".join(patch), encoding="utf-8", newline="\n")
        self.document = dict(schema="capsid-native-provider-files-v1",
            original={n: sha(v) for n, v in self.original.items()},
            patched={n: sha(v) for n, v in self.patched.items()})
        self.entry = dict(version="3.5.7", source=dict(url="https://example.invalid/provider.tar.gz",
            sha256="0" * 64, root="provider-3.5.7"), patch=self.patch.name, patchSHA256=sha(self.patch.read_bytes()),
            files=self.files.name, filesSHA256="", originalFiles=2, patchedFiles=3,
            changedFiles=["lib/callback.c"], addedFiles=["include/typed.h"])
        self.lock = dict(schema="capsid-qualified-native-providers-v1", providers=dict(openssl=self.entry),
            upstreamAssertionsAndRecipeSelectionPreserved=True,
            locallyPreparedCorrectionsNotUpstreamBackports=True, noSanitizerSuppressions=True)
        self.save()

    def save(self):
        self.files.write_text(json.dumps(self.document), encoding="utf-8", newline="\n")
        self.entry["filesSHA256"] = sha(self.files.read_bytes())
        self.manifest.write_text(json.dumps(self.lock), encoding="utf-8", newline="\n")

    def run_tool(self, *mode):
        return subprocess.run([sys.executable, "-I", "-B", str(TOOL), "--provider", "openssl",
            "--source", str(self.source), "--manifest", str(self.manifest), *mode],
            capture_output=True, text=True, encoding="utf-8", timeout=15)

    def source_bytes(self):
        return {p.relative_to(self.source).as_posix(): p.read_bytes() for p in self.source.rglob("*") if p.is_file()}

    def rejected_without_mutation(self, message, *mode):
        before = self.source_bytes()
        result = self.run_tool(*mode)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(message, result.stderr)
        self.assertEqual(self.source_bytes(), before, "refusal must precede source mutation")

    def test_dry_run_replays_patch_without_changing_source(self):
        result = self.run_tool("--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "original-source-and-patch-verified")
        self.assertEqual(self.source_bytes(), self.original)

    def test_apply_and_verify_preserve_the_original_test_bytes(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.source_bytes(), self.patched)
        verified = self.run_tool("--verify-only")
        self.assertEqual(verified.returncode, 0, verified.stderr)
        self.assertEqual(json.loads(verified.stdout)["sourceFiles"], 3)
        self.assertEqual((self.source / "test/original.c").read_bytes(), self.original["test/original.c"])

    def test_altered_upstream_file_is_rejected(self):
        (self.source / "lib/callback.c").write_bytes(b"different upstream revision\n")
        self.rejected_without_mutation("source manifest mismatch")

    def test_missing_upstream_file_is_rejected(self):
        (self.source / "lib/callback.c").unlink()
        self.rejected_without_mutation("source manifest mismatch")

    def test_preexisting_new_file_is_rejected(self):
        file = self.source / "include/typed.h"
        file.parent.mkdir()
        file.write_bytes(b"unrelated file\n")
        self.rejected_without_mutation("source manifest mismatch")

    def test_tampered_patch_is_rejected(self):
        self.patch.write_bytes(self.patch.read_bytes() + b"# corruption\n")
        self.rejected_without_mutation("provider patch digest mismatch")

    def test_inconsistent_result_hash_is_rejected_before_actual_apply(self):
        self.document["patched"]["lib/callback.c"] = "a" * 64
        self.save()
        self.rejected_without_mutation("source manifest mismatch")

    def test_rehashed_nonapplicable_patch_is_rejected_before_actual_apply(self):
        self.patch.write_bytes(self.patch.read_bytes().replace(b"-int callback", b"-long callback"))
        self.entry["patchSHA256"] = sha(self.patch.read_bytes())
        self.save()
        self.rejected_without_mutation("git apply failed")

    def test_manifest_path_traversal_is_rejected(self):
        self.document["original"]["../outside"] = self.document["original"].pop("lib/callback.c")
        self.save()
        self.rejected_without_mutation("unsafe relative file name")

    def test_patch_path_traversal_is_rejected(self):
        self.patch.write_bytes(self.patch.read_bytes().replace(b"+++ b/include/typed.h", b"+++ b/../../outside"))
        self.entry["patchSHA256"] = sha(self.patch.read_bytes())
        self.save()
        self.rejected_without_mutation("patch targets differ from the reviewed inventory")

    def test_reapplying_is_rejected_without_changing_patched_tree(self):
        applied = self.run_tool()
        self.assertEqual(applied.returncode, 0, applied.stderr)
        self.rejected_without_mutation("source manifest mismatch")

    def test_verify_detects_a_changed_patched_file(self):
        applied = self.run_tool()
        self.assertEqual(applied.returncode, 0, applied.stderr)
        (self.source / "lib/callback.c").write_bytes(b"tampered result\n")
        self.rejected_without_mutation("source manifest mismatch", "--verify-only")


if __name__ == "__main__":
    unittest.main()

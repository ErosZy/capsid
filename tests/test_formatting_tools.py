"""Exercise scope/guard behavior without formatting a captured program or vendor."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("capsid_format", ROOT / "tools/format.py")
FORMAT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FORMAT)
POLICY = json.loads((ROOT / "tools/format-policy.json").read_text(encoding="utf-8"))


class FormattingScopeTests(unittest.TestCase):
    def test_language_families_and_configuration(self):
        for name, language in [
            ("src/main.cc", "cpp"),
            ("include/capsid/runtime.h", "cpp"),
            ("js/fetch.js", "javascript"),
            ("tests/check.mjs", "javascript"),
            ("bench/probe.py", "python"),
            ("package-lock.json", "data"),
            ("examples/hono-reference/package.json", "data"),
        ]:
            with self.subTest(name=name):
                self.assertEqual(FORMAT.classify(name, POLICY), language)

    def test_locked_generated_third_party_and_evidence_are_protected(self):
        for name in [
            "vendor/txiki.js/src/main.c",
            "patches/txiki/code.js",
            "licenses/source.h",
            "bench/results/session/probe.py",
            "tests/fixtures/ordinary-fetch.mjs",
            "tests/wpt/adapter.js",
            "js/streams.js",
            "js/encoding-data/multibyte-indexes.js",
            "tools/encoding-data/gb18030.lock.json",
            "tools/encoding-data/captured.py",
            "docs/evidence/snapshot.py",
            "docs/history/archives/old.js",
            "perf-site/data.js",
            "perf-site/status.js",
            "bench/bench-apps/php-slim/vendor/private.py",
            "examples/hono-reference/node_modules/package/index.js",
        ]:
            with self.subTest(name=name):
                self.assertIsNone(FORMAT.classify(name, POLICY))

    def test_first_party_capture_inputs_keep_their_original_identities(self):
        for name, record in POLICY["preservedInputs"].items():
            with self.subTest(name=name):
                self.assertIsNone(FORMAT.classify(name, POLICY))
                # A live successor may change on purpose, but a capture's source
                # stays available at the original path inside its source archive.
                if archive := record.get("archive"):
                    self.assertIsNone(FORMAT.classify(archive, POLICY))
                    with ZipFile(ROOT / archive) as source:
                        self.assertEqual(source.namelist().count(name), 1)
                        original = source.read(name)
                else:
                    original = (ROOT / name).read_bytes()
                self.assertEqual(hashlib.sha256(original).hexdigest(), record["sha256"])
                self.assertTrue(record["references"])
                self.assertTrue(all((ROOT / ref).is_file() for ref in record["references"]))

    def test_exact_version_guard_rejects_similar_and_wrong_versions(self):
        FORMAT.verify_version([sys.executable, "-c", "print('ruff 0.17.5')"], "0.17.5")
        for reported in ["0.17.4", "10.17.5", "0.17.50"]:
            with (
                self.subTest(reported=reported),
                self.assertRaisesRegex(ValueError, "version mismatch"),
            ):
                FORMAT.verify_version([sys.executable, "-c", f"print('ruff {reported}')"], "0.17.5")

    def test_no_outside_root_or_unsupported_language(self):
        for name in ["../src/file.c", "/src/file.c", "README.md", "src/file.rs"]:
            self.assertIsNone(FORMAT.classify(name, POLICY))

    def test_git_selection_excludes_ignored_files_and_protects_explicit_targets(self):
        with tempfile.TemporaryDirectory(prefix="capsid-format-scope-") as directory:
            root = Path(directory)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            for name, value in {
                ".gitignore": "node_modules/\n",
                "src/a.cc": "int a;\n",
                "src/node_modules/hidden.js": "invalid dependency\n",
                "tests/fixtures/sealed.mjs": "sealed bytes\r\n",
                "bench/probe.py": "print(1)\n",
            }.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(value.encode())
            selected = FORMAT.select_files(root, POLICY)
            self.assertEqual(selected["cpp"], ["src/a.cc"])
            self.assertEqual(selected["python"], ["bench/probe.py"])
            self.assertEqual(selected["javascript"], [])
            with self.assertRaisesRegex(ValueError, "protected"):
                FORMAT.select_files(root, POLICY, ["tests/fixtures/sealed.mjs"])
            with self.assertRaisesRegex(ValueError, "Git-listed"):
                FORMAT.select_files(root, POLICY, ["src/absent.cc"])
            with self.assertRaisesRegex(ValueError, "empty"):
                FORMAT.select_files(root, POLICY, language="javascript")
            self.assertEqual((root / "tests/fixtures/sealed.mjs").read_bytes(), b"sealed bytes\r\n")

    def test_checkout_preserves_raw_locked_json_under_both_autocrlf_modes(self):
        attributes = (ROOT / ".gitattributes").read_bytes()
        name = "tools/encoding-data/gb18030.lock.json"
        raw = b'{\r\n  "locked": true\r\n}\r\n'
        for autocrlf in ["false", "true"]:
            with (
                self.subTest(autocrlf=autocrlf),
                tempfile.TemporaryDirectory(prefix="capsid-format-eol-") as directory,
            ):
                root = Path(directory)

                def git(*args, data=None):
                    return subprocess.run(
                        [
                            "git",
                            "-C",
                            str(root),
                            "-c",
                            f"core.autocrlf={autocrlf}",
                            "-c",
                            "gc.auto=0",
                            "-c",
                            "user.name=Formatting control",
                            "-c",
                            "user.email=formatting@example.invalid",
                            *args,
                        ],
                        input=data,
                        capture_output=True,
                        check=True,
                    ).stdout

                git("init", "-q")
                (root / ".gitattributes").write_bytes(attributes)
                path = root / name
                path.parent.mkdir(parents=True)
                path.write_bytes(raw)
                git("add", "--", ".gitattributes")
                # A legacy lock must enter the index verbatim, not through the
                # EOL rule being tested. This models the already-published blob.
                oid = git("hash-object", "-w", "--stdin", data=raw).decode().strip()
                git("update-index", "--add", "--cacheinfo", f"100644,{oid},{name}")
                git("commit", "-qm", "retain original lock bytes")
                git("checkout-index", "--all", "--force")
                self.assertEqual(path.read_bytes(), raw)
                self.assertEqual(git("status", "--porcelain"), b"")

                self.assertEqual(git("hash-object", "--path", name, name).decode().strip(), oid)

                # Git compares the unchanged raw CRLF blob through an LF clean
                # filter. It can therefore report a fresh checkout as dirty.
                (root / ".gitattributes").write_bytes(
                    attributes + b"\n/tools/encoding-data/*.json text eol=lf\n"
                )
                git("add", "--", ".gitattributes")
                git("commit", "-qm", "negative EOL control")
                git("checkout-index", "--all", "--force")
                self.assertEqual(path.read_bytes(), raw)
                self.assertNotEqual(git("hash-object", "--path", name, name).decode().strip(), oid)
                stamp = path.stat()
                os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns + 1_000_000_000))
                self.assertIn(name.encode(), git("status", "--porcelain"))

                (root / ".gitattributes").write_bytes(attributes)
                git("add", "--", ".gitattributes")
                git("commit", "-qm", "restore protected lock")
                git("checkout-index", "--all", "--force")
                self.assertEqual(path.read_bytes(), raw)
                self.assertEqual(git("status", "--porcelain"), b"")

    def test_batches_preserve_exact_inventory_and_bound_command_length(self):
        names = [f"tests/file with spaces-{index}.mjs" for index in range(40)]
        groups = list(FORMAT.batches(names, maximum=150))
        self.assertEqual([name for group in groups for name in group], names)
        self.assertTrue(all(sum(len(name) + 3 for name in group) <= 150 for group in groups))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Parser/publication controls. Actual vendor application is verified separately."""
from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[1] / "tools/generate-txiki-upgrade-report.py"
SPEC = importlib.util.spec_from_file_location("txiki_upgrade_report", SOURCE)
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


class VendorContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source"
        self.build = Path(self.temp.name) / "build"
        (self.root / "vendor/txiki.js").mkdir(parents=True)
        (self.root / "patches/txiki").mkdir(parents=True)
        for index in range(1, 72):
            (self.root / f"patches/txiki/{index:04d}-fixture.patch").write_text(f"fixture {index}\n")
        (self.root / "cmake").mkdir()
        for name in ["AuditTxikiVendor.cmake", "PrepareTxiki.cmake", "ComputeTxikiOverlayKey.cmake"]:
            (self.root / "cmake" / name).write_text("canonical audit command fixture\n")
        self.stamp = self.build / "vendor-overlay/txiki.js/.capsid-overlay-key"
        self.stamp.parent.mkdir(parents=True)
        self.stamp.write_text("schema=capsid-txiki-overlay-stamp-v1\nkey=" + "1" * 64 + "\nmanifest=" + "2" * 64 + "\n")
        self.commands = []

    def run_command(self, args, cwd=None):
        self.commands.append(args)
        if args[0] == "git":
            return {"describe": "v0.fixture", "rev-parse": "a" * 40,
                    "status": "", "submodule": "-bbbb deps/quickjs/test262"}[args[1]]
        self.assertEqual(args[0], "cmake")
        self.assertEqual(args[-2:], ["-P", str(self.root / "cmake/AuditTxikiVendor.cmake")])
        self.assertIn(f"-DCAPSID_TXIKI_PREPARE_SCRIPT={self.root / 'cmake/PrepareTxiki.cmake'}", args)
        return "canonical CMake audit fixture completed"

    def evidence(self, command=None):
        with patch.object(report, "run", side_effect=command or self.run_command):
            return report.vendor_evidence(self.root, self.build)

    def test_current_patch_list_calls_one_canonical_audit_and_no_second_patch_tool(self):
        value = self.evidence()
        self.assertEqual(value["patch_count"], 71)
        self.assertEqual(len(value["patches"]), 71)
        self.assertEqual(value["combined_apply_check"], "passed")
        self.assertTrue(value["audit"]["passed"])
        self.assertEqual(sum(args[0] == "cmake" for args in self.commands), 1)
        self.assertFalse(any(args[0] == "patch" for args in self.commands))

    def test_failed_canonical_audit_cannot_claim_success(self):
        def run(args, cwd=None):
            if args[0] == "cmake":
                raise subprocess.CalledProcessError(1, args, output="canonical rejection")
            return self.run_command(args, cwd)
        with self.assertRaises(subprocess.CalledProcessError):
            self.evidence(run)

    def test_changing_a_patch_or_stamp_during_audit_is_rejected(self):
        for path in [self.root / "patches/txiki/0071-fixture.patch", self.stamp]:
            with self.subTest(path=path):
                before = path.read_bytes()
                def run(args, cwd=None):
                    result = self.run_command(args, cwd)
                    if args[0] == "cmake":
                        path.write_text("changed during audit")
                    return result
                with self.assertRaisesRegex(RuntimeError, "changed during audit"):
                    self.evidence(run)
                path.write_bytes(before)

    def test_bad_source_prerequisites_cannot_reach_the_apply_audit(self):
        for kind in ["dirty", "submodule", "stamp"]:
            with self.subTest(kind=kind):
                def run(args, cwd=None):
                    if kind == "dirty" and args[:2] == ["git", "status"]:
                        return " M tracked.c"
                    if kind == "submodule" and args[:2] == ["git", "submodule"]:
                        return "+bbbb deps/quickjs"
                    return self.run_command(args, cwd)
                before = self.stamp.read_bytes()
                if kind == "stamp":
                    self.stamp.write_text("invalid stamp")
                self.commands.clear()
                with self.assertRaises(RuntimeError):
                    self.evidence(run)
                self.assertFalse(any(args[0] == "cmake" for args in self.commands))
                self.stamp.write_bytes(before)


class CTestContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.junit = self.build / "full.junit.xml"
        names = sorted(report.REQUIRED_AUDIT_TESTS) + [f"worker_wpt_fixture_{i}" for i in range(80)] + ["optional"]
        self.catalog = {"tests": [{"name": name, "properties": []} for name in names]}
        self.xml = ET.Element("testsuite")
        for name in names:
            ET.SubElement(self.xml, "testcase", name=name, status="run", time="0.1")
        self.summary()

    def summary(self):
        cases = self.xml.findall("testcase")
        self.xml.attrib.update(tests=str(len(cases)), failures=str(sum(c.find("failure") is not None for c in cases)),
                               skipped=str(sum(c.find("skipped") is not None for c in cases)), disabled="0")

    def evidence(self):
        ET.ElementTree(self.xml).write(self.junit, encoding="utf-8", xml_declaration=True)
        with patch.object(report, "run", return_value=json.dumps(self.catalog)):
            return report.ctest_evidence(self.build, self.junit)

    def test_complete_catalog_binds_every_case_and_required_audit(self):
        value = self.evidence()
        self.assertEqual(value["registered"], len(self.catalog["tests"]))
        self.assertEqual(value["passed"], len(self.catalog["tests"]))
        self.assertEqual(value["wpt_files"], 80)

    def test_reject_missing_duplicate_and_extra_evidence(self):
        original_xml, original_catalog = copy.deepcopy(self.xml), copy.deepcopy(self.catalog)
        for kind in ["duplicate-catalog", "duplicate-junit", "missing", "extra", "empty", "unpinned"]:
            with self.subTest(kind=kind):
                self.xml, self.catalog = copy.deepcopy(original_xml), copy.deepcopy(original_catalog)
                if kind == "duplicate-catalog":
                    self.catalog["tests"].append(self.catalog["tests"][0])
                elif kind == "duplicate-junit":
                    duplicate = ET.Element("testcase", name=self.xml[0].get("name"), status="fail", time="1")
                    ET.SubElement(duplicate, "failure", message="earlier failure hidden by later passing copy")
                    self.xml.insert(0, duplicate)
                elif kind == "missing":
                    self.xml.remove(self.xml[-1])
                elif kind == "extra":
                    ET.SubElement(self.xml, "testcase", name="unregistered", status="run", time="1")
                elif kind == "empty":
                    self.catalog["tests"] = []
                else:
                    self.catalog["tests"].append({"name": "wpt_conformance_not_configured"})
                self.summary()
                with self.assertRaises(RuntimeError):
                    self.evidence()

    def test_summary_must_agree_with_execution(self):
        for key in ["tests", "failures", "skipped", "disabled"]:
            with self.subTest(key=key):
                self.summary()
                self.xml.set(key, "9999")
                with self.assertRaisesRegex(RuntimeError, "summary"):
                    self.evidence()

    def test_status_and_time_cannot_invent_execution(self):
        case = self.xml[-1]
        for status, elapsed in [(None, "1"), ("notrun", "1"), ("run", None),
                                ("run", "NaN"), ("run", "inf"), ("run", "-1")]:
            with self.subTest(status=status, elapsed=elapsed):
                case.attrib = {"name": "optional"}
                if status is not None:
                    case.set("status", status)
                if elapsed is not None:
                    case.set("time", elapsed)
                with self.assertRaises(RuntimeError):
                    self.evidence()

    def test_environment_skip_is_preserved_and_requires_declared_prerequisite(self):
        self.xml[-1].set("status", "notrun")
        ET.SubElement(self.xml[-1], "skipped")
        self.summary()
        with self.assertRaisesRegex(RuntimeError, "unexpected_skips"):
            self.evidence()
        self.catalog["tests"][-1]["properties"] = [{"name": "SKIP_RETURN_CODE", "value": 77}]
        value = self.evidence()
        self.assertEqual(value["skipped"], ["optional"])
        self.assertEqual(value["passed"], len(self.catalog["tests"]) - 1)

    def test_standard_failure_is_not_waived_and_required_audit_cannot_skip(self):
        case = self.xml[0]
        case.set("status", "fail")
        ET.SubElement(case, "failure", message="actual failure")
        self.summary()
        with self.assertRaisesRegex(RuntimeError, "full matrix failed"):
            self.evidence()
        case.remove(case.find("failure"))
        case.set("status", "notrun")
        ET.SubElement(case, "skipped")
        self.catalog["tests"][0]["properties"] = [{"name": "SKIP_RETURN_CODE", "value": 77}]
        self.summary()
        with self.assertRaisesRegex(RuntimeError, "required audit"):
            self.evidence()


class PublicationContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        directory = Path(self.temp.name)
        self.args = argparse.Namespace(source_root=directory, build_dir=directory / "build",
            junit=directory / "full.xml", baseline=directory / "baseline.json",
            output_json=directory / "report.json", output_md=directory / "report.md")
        self.args.output_json.write_text('{"state":"complete","old":true}')
        self.args.output_md.write_text("old successful report")
        self.args.baseline.write_text("original input")

    def test_failure_and_interruption_cannot_leave_an_old_complete_report(self):
        for error, state in [(RuntimeError("proof rejected"), "failed"), (KeyboardInterrupt(), "incomplete")]:
            with self.subTest(state=state):
                def fail(args):
                    self.assertEqual(json.loads(args.output_json.read_text())["state"], "incomplete")
                    raise error
                with patch.object(report, "parse_args", return_value=self.args), patch.object(report, "collect_report", side_effect=fail):
                    with self.assertRaises(type(error)):
                        report.main()
                self.assertEqual(json.loads(self.args.output_json.read_text())["state"], state)
                self.assertNotIn("old successful", self.args.output_md.read_text(encoding="utf-8"))

    def test_input_alias_and_duplicate_outputs_are_rejected_before_writing(self):
        for kind in ["input", "same-output"]:
            with self.subTest(kind=kind):
                args = copy.copy(self.args)
                if kind == "input":
                    args.output_json = args.baseline
                else:
                    args.output_md = args.output_json
                with patch.object(report, "parse_args", return_value=args):
                    with self.assertRaisesRegex(RuntimeError, "distinct"):
                        report.main()
                self.assertEqual(args.baseline.read_text(), "original input")

    def test_complete_is_written_only_after_collection_returns(self):
        value = {"schema_version": 1, "state": "complete", "parser_fixture": True}
        with patch.object(report, "parse_args", return_value=self.args), patch.object(report, "collect_report", return_value=value), \
                patch.object(report, "render_report", return_value="complete parser fixture report"):
            report.main()
        self.assertEqual(json.loads(self.args.output_json.read_text()), value)


if __name__ == "__main__":
    unittest.main(verbosity=2)

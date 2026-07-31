import importlib.util
import pathlib
import unittest


PATH = pathlib.Path(__file__).with_name(
    "generate-txiki-upgrade-report.py"
)
SPEC = importlib.util.spec_from_file_location("txiki_report", PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class TxikiUpgradeReportTest(unittest.TestCase):
    def test_cache_parser(self):
        path = pathlib.Path(self.id().replace(".", "-") + ".cache")
        try:
            path.write_text(
                "# comment\nA:BOOL=ON\nB:FILEPATH=/tmp/value\n",
                encoding="utf-8",
            )
            self.assertEqual(
                MODULE.cache_values(path),
                {"A": "ON", "B": "/tmp/value"},
            )
        finally:
            path.unlink(missing_ok=True)

    def test_required_audits_cover_both_positive_and_negative(self):
        self.assertIn(
            "worker_binary_audit",
            MODULE.REQUIRED_AUDIT_TESTS,
        )
        self.assertIn(
            "worker_binary_audit_negative_controls",
            MODULE.REQUIRED_AUDIT_TESTS,
        )
        self.assertIn(
            "txiki_overlay_audit_negative_controls",
            MODULE.REQUIRED_AUDIT_TESTS,
        )

    def test_surface_inventory_uses_capsid_public_modules(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        inventory = MODULE.surface_inventory(root)
        visible = inventory["modules"]["application_visible"]
        self.assertTrue(visible)
        self.assertTrue(
            all(module.startswith("capsid:") for module in visible)
        )


if __name__ == "__main__":
    unittest.main()

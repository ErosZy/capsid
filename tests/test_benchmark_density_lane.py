#!/usr/bin/env python3
"""Producer contract tests; synthetic probes never leave the temp repository."""
import argparse
import importlib.util
import json
import os
import signal
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ap = argparse.ArgumentParser(add_help=False)
ap.add_argument("--lane", type=Path, default=Path(__file__).resolve().parents[1] / "bench/density-lane.py")
args, rest = ap.parse_known_args()
sys.argv[1:] = rest
sys.path.insert(0, str(args.lane.resolve().parent))
spec = importlib.util.spec_from_file_location("density_lane", args.lane)
lane = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lane)
from measurement_runtime import ProbeError
from perf_evidence import Archive, sha256
sys.path.insert(0, str(Path(__file__).resolve().parent / 'helpers'))
from density_measurement_fixture import synthetic_density


class PreflightTests(unittest.TestCase):
    def test_formal_missing_metadata_or_nonlinux_never_launches_a_probe(self):
        with tempfile.TemporaryDirectory(prefix="capsid-density-preflight-") as temp:
            root = Path(temp)
            marker = root / "unexpected-launch"
            executable = root / "fake-executable"
            executable.write_text("#!/usr/bin/env python3\nfrom pathlib import Path\nPath(" + repr(str(marker)) + ").touch()\n")
            executable.chmod(0o700)
            env = {k: v for k, v in os.environ.items() if k not in (
                "OUT", "CAPSID_BENCH_METADATA", "CAPSID_MANAGED", "CAPSID_STRICT", "STACKS", "SIZES")}
            command = [sys.executable, "-B", str(args.lane.resolve()), "--host", str(executable),
                       "--worker", str(executable), "--workerd", str(executable), "--out", str(root / "out")]
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertIn("formal density requires" if sys.platform == "linux" else "requires Linux", result.stderr)
            self.assertFalse(marker.exists())
            self.assertFalse((root / "out").exists())
            if sys.platform == "linux":
                result = subprocess.run(command + ["--observation"], capture_output=True, text=True,
                                        env=dict(env, CAPSID_STRICT="0"), timeout=10)
                self.assertEqual(result.returncode, 2)
                self.assertIn("always uses managed/strict", result.stderr)
                self.assertFalse(marker.exists())


@unittest.skipUnless(sys.platform == "linux", "formal producer/PSS lanes require Linux")
class ProducerTests(unittest.TestCase):
    def setUp(self):
        import perf_run
        self.perf_run = perf_run
        self.temp = tempfile.TemporaryDirectory(prefix="capsid-density-producer-")
        self.repo = Path(self.temp.name)
        # Only the synthetic fixture's lock path is private. Its existing
        # overlap test still exercises the real nonblocking file lock.
        lock_directory = patch.object(perf_run, 'temp_directory', return_value=str(self.repo))
        lock_directory.start()
        self.addCleanup(lock_directory.stop)
        self.source = self.repo / "source"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text("project(capsid_runtime VERSION test LANGUAGES C)\n")
        git_env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        git_env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        def git(*values):
            run = subprocess.run(["git", "-C", str(self.source), *values], capture_output=True, text=True, env=git_env)
            self.assertEqual(run.returncode, 0, run.stderr)
            return run.stdout.strip()
        git("init", "--template=")
        git("add", "CMakeLists.txt")
        git("-c", "user.name=Capsid synthetic fixture", "-c", "user.email=fixture@capsid.invalid",
            "-c", "commit.gpgsign=false", "commit", "-m", "Synthetic producer contract fixture")
        commit = git("rev-parse", "HEAD")
        self.origin = {"commit": commit, "tag": "", "version": "test", "release": "v-test", "clean": True}
        self.paths = {}
        self.binaries = {}
        for name in ("capsid-host", "capsid-worker", "workerd"):
            file = self.repo / name
            file.write_bytes(("not a real benchmark executable: " + name).encode())
            self.paths[name] = file
            self.binaries[name] = {"commit": commit, "tag": "", "version": "test", "sha256": sha256(file)}
        self.metadata = {"schema": 1, "session": "synthetic-producer-test", "quietSession": True,
                         "sourceRoot": str(self.source), "origin": self.origin,
                         "machine": perf_run.machine_identity(), "binaries": self.binaries}
        self.meta_file = self.repo / "metadata.json"
        self.meta_file.write_text(json.dumps(self.metadata))
        self.apps = self.repo / "apps"
        generated = subprocess.run([sys.executable, "-B", str(args.lane.with_name("gen-cold-start-apps.py")), str(self.apps)],
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.configuration = SimpleNamespace(out=self.repo / "bench/results/synthetic", repo=self.repo,
            host=self.paths["capsid-host"], worker=self.paths["capsid-worker"], workerd=self.paths["workerd"],
            apps=self.apps, cpuset="0-3", sizes=["10k", "100k", "1m"], stacks=["capsid", "workerd"],
            budget_mb=2048, max_workers=1, max_cells=1, warm_requests=100, parallel=64,
            metadata=self.meta_file, observation=False, cell_deadline=10)

    def tearDown(self):
        self.temp.cleanup()

    def test_complete_synthetic_producer_archive_passes_the_real_validator(self):
        # Only the SUT probe is injected. Real metadata/source checks, port
        # leases, search, artifact writer, sealing and validator all execute.
        created = []
        case, shared_apps = self, self.apps
        expected_inputs = {p.name: p.read_bytes() for p in self.apps.glob("*.mjs")}
        class SyntheticProbe:
            def __init__(self, **options):
                self.options = options
            def __call__(self, **call):
                created.append(call)
                call["release_ports"]()
                if call["probe_index"] == 1:
                    (shared_apps / "app-workerd-10k.mjs").write_text("changed after input snapshot")
                directory = f"synthetic-probe-{call['probe_index']}"
                wrapper = ("runtime/apps/orders/v1/bundle.mjs" if call["stack"] == "capsid" else "runtime/bundle.mjs")
                file = self.options["out"] / directory / wrapper
                file.parent.mkdir(parents=True)
                source = self.options["apps"] / f"app-{call['stack']}-{call['size']}.mjs"
                data = source.read_bytes()
                case.assertEqual(data, expected_inputs[source.name], "shared input mutation changed the active measurement")
                row, _ = synthetic_density(self.options['out'], directory, call['stack'], call['size'],
                    call['phase'], call['count'], probe_index=call['probe_index'], source=data.decode('utf-8'),
                    final=call['final'], ports=call['ports'])
                return row
        with patch.object(lane, "DensityProbe", SyntheticProbe):
            result = lane.run_lane(self.configuration)
        self.assertTrue(result["publicationEligible"], "contract fixture only, never an actual performance point")
        self.assertEqual(result["cells"], 12)
        self.assertEqual(len(created), 24)
        archive = Archive(self.configuration.out, self.repo)
        self.assertEqual(archive.manifest["origin"], self.origin)
        self.assertEqual(len(archive.manifest["records"]), 12)
        self.assertEqual(archive.manifest["protocol"]["caps"], {"capsid": 1, "workerd": 1})
        for file, digest in archive.manifest["runner"]["files"].items():
            self.assertEqual(sha256(self.configuration.out / file), digest)
        self.assertIn("python", archive.manifest["runner"]["tools"])
        self.assertFalse((self.repo / "data.js").exists())
        archive.recheck()

    def test_bad_binary_machine_and_dirty_source_are_rejected_before_any_probe(self):
        for field in ("binary", "machine", "source"):
            with self.subTest(field=field):
                original = self.meta_file.read_bytes()
                changed = json.loads(original)
                dirty = self.source / "untracked"
                if field == "binary":
                    changed["binaries"]["workerd"]["sha256"] = "0" * 64
                elif field == "machine":
                    changed["machine"]["kernel"] = "wrong kernel"
                else:
                    dirty.write_text("dirty source")
                self.meta_file.write_text(json.dumps(changed))
                try:
                    with patch.object(lane, "DensityProbe") as adapter, self.assertRaises(ProbeError):
                        lane.run_lane(self.configuration)
                    adapter.assert_not_called()
                    self.assertFalse(self.configuration.out.exists())
                finally:
                    self.meta_file.write_bytes(original)
                    dirty.unlink(missing_ok=True)

    def test_session_lock_rejects_overlap_and_is_released(self):
        with self.perf_run.session_lock(True):
            with self.assertRaises(ProbeError) as raised:
                with self.perf_run.session_lock(True):
                    self.fail("concurrent formal session accepted")
            self.assertEqual(raised.exception.code, "session-overlap")
        with self.perf_run.session_lock(True) as state:
            self.assertTrue(state["held"])

    def test_output_creation_race_does_not_write_into_an_unowned_archive(self):
        self.configuration.observation = True
        self.configuration.out.mkdir(parents=True)
        marker = self.configuration.out / "other-run.log"
        marker.write_bytes(b"evidence from another controller")
        handlers = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            # The directory appears after preflight but before atomic mkdir.
            with patch.object(lane, "parse_args", return_value=self.configuration), self.assertRaises(FileExistsError):
                lane.main([])
            self.assertEqual({p.name for p in self.configuration.out.iterdir()}, {"other-run.log"})
            self.assertEqual(marker.read_bytes(), b"evidence from another controller")
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)


if __name__ == "__main__":
    unittest.main()

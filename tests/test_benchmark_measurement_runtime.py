#!/usr/bin/env python3
"""Model/port tests on both hosts, actual owned-process tests on Linux.

The peer modes create only this test's processes and temporary records. They
are not production sampler options and never generate performance data.
"""
import argparse
from contextlib import redirect_stdout
import ctypes
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ap = argparse.ArgumentParser(add_help=False)
ap.add_argument("--runtime", type=Path, default=Path(__file__).resolve().parents[1] / "bench/measurement_runtime.py")
ap.add_argument("--peer", action="store_true")
ap.add_argument("--controller", type=int)
ap.add_argument("--group-controller", action="store_true")
ap.add_argument("--records", type=Path)
ap.add_argument("--role", default="parent")
ap.add_argument("--parent-exits", action="store_true")
ap.add_argument("--reap-children", action="store_true")
args, rest = ap.parse_known_args()
sys.argv[1:] = rest
spec = importlib.util.spec_from_file_location("measurement_runtime", args.runtime)
runtime = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runtime
spec.loader.exec_module(runtime)


def peer_command(records, *options):
    return [sys.executable, "-B", "-u", str(Path(__file__).resolve()), "--runtime", str(args.runtime.resolve()),
            "--peer", "--records", str(records), *options]


def read_records(path):
    try:
        return [json.loads(line) for line in path.read_text().splitlines()]
    except FileNotFoundError:
        return []


def wait_for(predicate, timeout=4):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            return False
        time.sleep(.01)
    return True


def run_peer():
    if args.reap_children:
        def reap_children(*_):
            while True:
                try:
                    pid, _ = os.waitpid(-1, os.WNOHANG)
                except ChildProcessError:
                    return
                if pid == 0:
                    return
        signal.signal(signal.SIGCHLD, reap_children)
    identity = runtime.ProcFS().identity(os.getpid())
    payload = (json.dumps({"role": args.role, "pid": identity.pid, "start": identity.start}) + "\n").encode()
    descriptor = os.open(args.records, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(descriptor, payload)
    finally:
        os.close(descriptor)
    if args.role != "grandchild":
        role = "child" if args.role == "parent" else "grandchild"
        subprocess.Popen(peer_command(args.records, "--role", role))
    if args.parent_exits:
        if not wait_for(lambda: len(read_records(args.records)) == 3):
            sys.exit(2)
        os._exit(0)
    while True:
        time.sleep(1)


def run_controller():
    def interrupted(signum, _):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    with (args.records.parent / "controller.log").open("wb") as output:
        if args.group_controller:
            tree = runtime.OwnedGroup()
            tree.spawn(peer_command(args.records), stdout=output)
            tree.spawn(peer_command(args.records), stdout=output)
        else:
            tree = runtime.OwnedProcess(peer_command(args.records), stdout=output)
        try:
            while True:
                time.sleep(.05)
        finally:
            report = tree.close()
            (args.records.parent / "cleanup.json").write_text(json.dumps(report))


if args.peer:
    run_peer()
    sys.exit(0)
if args.controller is not None:
    run_controller()
    sys.exit(0)


I = runtime.ProcessIdentity


class ModelTests(unittest.TestCase):
    def test_recursive_ancestry_session_and_foreign_processes(self):
        root = I(10, 100, 1, 10, "S")
        snapshot = {10: root, 11: I(11, 101, 10, 10, "S"),
                    12: I(12, 102, 11, 10, "S"), 13: I(13, 103, 11, 13, "S"),
                    21: I(21, 105, 1, 21, "S")}
        owned = runtime.owned_identities(root, {10: root}, snapshot)
        self.assertEqual(set(owned), {10, 11, 12, 13})
        order = [p.pid for p in runtime.postorder(owned, 10)]
        self.assertLess(order.index(12), order.index(11))
        self.assertLess(order.index(13), order.index(11))
        self.assertEqual(order[-1], 10)

    def test_orphans_keep_session_and_reused_foreign_pid_is_excluded(self):
        root, child = I(10, 100, 1, 10, "Z"), I(11, 101, 10, 10, "S")
        snapshot = {10: root, 11: I(11, 999, 1, 11, "S"), 12: I(12, 102, 1, 10, "S")}
        result = runtime.owned_identities(root, {10: root, 11: child}, snapshot)
        self.assertEqual(set(result), {10, 12})
        snapshot[10] = I(10, 999, 1, 10, "S")
        with self.assertRaisesRegex(runtime.ProbeError, "reused"):
            runtime.owned_identities(root, {10: root}, snapshot)

    def test_proc_stat_parentheses_and_pss_errors(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp) / "42"
            folder.mkdir()
            values = ["S", "2", "42", "42", *(["0"] * 15), "123"]
            (folder / "stat").write_text("42 (odd ) process) " + " ".join(values))
            proc = runtime.ProcFS(Path(temp))
            identity = proc.identity(42)
            self.assertEqual(identity, I(42, 123, 2, 42, "S"))
            self.assertIsNone(proc.identity(99))
            for content, code in (("", "pss-format"), ("Pss: 0 kB\n", "pss-nonpositive"),
                                  ("Pss: nan kB\n", "pss-format"), ("Pss: 1 MB\n", "pss-format")):
                with self.subTest(content=content):
                    (folder / "smaps_rollup").write_text(content)
                    with self.assertRaises(runtime.ProbeError) as raised:
                        proc.pss_kb(identity)
                    self.assertEqual(raised.exception.code, code)
            (folder / "smaps_rollup").write_text("Rss: 16 kB\nPss: 12 kB\n")
            self.assertEqual(proc.pss_kb(identity), 12)
            (folder / "smaps_rollup").unlink()
            with self.assertRaises(runtime.ProbeError) as raised:
                proc.pss_kb(identity)
            self.assertEqual(raised.exception.code, "pss-unreadable")

    def test_two_equal_positive_samples_and_bounded_drift(self):
        def sample(value, pid=1):
            return {"pss_kb": value, "processes": [{"pid": pid, "start": 7, "pss_kb": value}]}
        iterator = iter([sample(10), sample(11), sample(11)])
        delays = []
        result = runtime.stable_pss(lambda: next(iterator), interval=.25, sleep=delays.append)
        self.assertEqual(result["pss_readings_kb"], [11, 11])
        self.assertEqual(len(result["samples"]), 3)
        self.assertEqual(delays, [.25, .25])
        count = 0
        def drift():
            nonlocal count
            count += 1
            return sample(count)
        with self.assertRaises(runtime.ProbeError) as raised:
            runtime.stable_pss(drift, attempts=4, sleep=lambda _: None)
        self.assertEqual(raised.exception.code, "pss-unstable")
        self.assertEqual(count, 4)
        iterator = iter([sample(10), sample(10, pid=2)])
        with self.assertRaises(runtime.ProbeError) as raised:
            runtime.stable_pss(lambda: next(iterator), sleep=lambda _: None)
        self.assertEqual(raised.exception.code, "process-changed")

    def test_invalid_samples_and_read_failures_never_become_zero(self):
        for value in (None, {}, {"pss_kb": 0, "processes": []},
                      {"pss_kb": True, "processes": [{}]},
                      {"pss_kb": 1, "processes": [{"pid": 1, "start": 7, "pss_kb": 2}]}):
            with self.subTest(value=value), self.assertRaises(runtime.ProbeError):
                runtime.stable_pss(lambda: value, sleep=lambda _: None)
        def denied():
            raise PermissionError("test denied")
        with self.assertRaises(runtime.ProbeError) as raised:
            runtime.stable_pss(denied)
        self.assertEqual(raised.exception.record()["status"], "error")
        self.assertEqual(raised.exception.code, "pss-unreadable")

    def test_port_state_survives_stdout_capture_and_concurrent_allocators(self):
        allocator = runtime.PortAllocator()
        with redirect_stdout(io.StringIO()):
            first = allocator.allocate(2)
        ports = set(first.ports)
        with runtime.PortAllocator().allocate(2) as other:
            self.assertFalse(ports & set(other.ports))
        first.release()
        with redirect_stdout(io.StringIO()):
            second = allocator.allocate(2)
        try:
            self.assertFalse(ports & set(second.ports))
        finally:
            second.release()

    def test_occupied_port_fails_without_closing_foreign_listener(self):
        with socket.socket() as foreign:
            foreign.bind(("127.0.0.1", 0))
            foreign.listen()
            port = foreign.getsockname()[1]
            with self.assertRaises(runtime.ProbeError) as raised:
                runtime.PortAllocator().allocate(1, requested=[port])
            self.assertEqual(raised.exception.code, "port-in-use")
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                accepted, _ = foreign.accept()
                accepted.close()


@unittest.skipUnless(sys.platform == "linux", "actual /proc process/PSS qualification is Linux-only")
class LinuxProcessTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="capsid-measurement-process-")
        self.folder = Path(self.temp.name)
        self.records = self.folder / "processes.jsonl"
        self.proc = runtime.ProcFS()

    def tearDown(self):
        self.temp.cleanup()

    def identities_gone(self, records):
        return all((current := self.proc.identity(p["pid"])) is None or current.start != p["start"]
                   for p in records)

    def test_constructor_failure_after_spawn_still_owns_actual_descendants(self):
        for error_type in (PermissionError, KeyboardInterrupt):
            with self.subTest(error=error_type.__name__):
                records_path = self.folder / (error_type.__name__ + ".jsonl")
                reaper = runtime.Subreaper()
                class InterruptedIdentity(runtime.ProcFS):
                    fired = False
                    def identity(inner, pid):
                        if not inner.fired:
                            self.assertTrue(wait_for(lambda: len(read_records(records_path)) == 3))
                            inner.fired = True
                            raise error_type("injected interruption after spawn")
                        return super().identity(pid)
                observed = []
                try:
                    with (self.folder / "constructor.log").open("wb") as output:
                        with self.assertRaises(error_type):
                            runtime.OwnedProcess(peer_command(records_path), stdout=output, proc=InterruptedIdentity())
                    observed = read_records(records_path)
                    self.assertEqual(len(observed), 3)
                    self.assertTrue(self.identities_gone(observed), "constructor abandoned an already started descendant")
                finally:
                    observed = observed or read_records(records_path)
                    for process in observed:
                        current = self.proc.identity(process["pid"])
                        if current is not None and current.start == process["start"]:
                            try:
                                os.kill(process["pid"], signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                    for _ in range(100):
                        for process in observed:
                            try:
                                os.waitpid(process["pid"], os.WNOHANG)
                            except ChildProcessError:
                                pass
                        if self.identities_gone(observed):
                            break
                        time.sleep(.01)
                    reaper.close()

    def test_actual_grandchildren_pss_and_postorder_reaping_preserve_foreign_child(self):
        with (self.folder / "peer.log").open("wb") as output:
            tree = runtime.OwnedProcess(peer_command(self.records), stdout=output)
            foreign = subprocess.Popen([sys.executable, "-B", "-c", "import time;time.sleep(30)"],
                                       start_new_session=True)
            try:
                self.assertTrue(wait_for(lambda: len(read_records(self.records)) == 3))
                records = read_records(self.records)
                sample = tree.sample()
                self.assertEqual({p["pid"] for p in sample["processes"]}, {p["pid"] for p in records})
                self.assertGreater(sample["pss_kb"], 0)
                report = tree.close()
                self.assertEqual(report["ownedPidsRemaining"], 0)
                self.assertTrue(self.identities_gone(records))
                kills = [p["pid"] for p in report["signals"] if p["signal"] == "SIGKILL"]
                roles = {p["role"]: p["pid"] for p in records}
                self.assertLess(kills.index(roles["grandchild"]), kills.index(roles["child"]))
                self.assertLess(kills.index(roles["child"]), kills.index(roles["parent"]))
                self.assertNotIn(foreign.pid, kills)
                self.assertIsNone(foreign.poll(), "unrelated child survives owned cleanup")
            finally:
                tree.close()
                foreign.terminate()
                foreign.wait(timeout=3)

    def test_parent_exit_is_failure_and_orphaned_grandchildren_are_reaped(self):
        with (self.folder / "peer.log").open("wb") as output:
            tree = runtime.OwnedProcess(peer_command(self.records, "--parent-exits"), stdout=output)
            try:
                self.assertTrue(wait_for(lambda: len(read_records(self.records)) == 3 and not tree.alive()))
                records = read_records(self.records)
                with self.assertRaises(runtime.ProbeError) as raised:
                    tree.sample()
                self.assertEqual(raised.exception.code, "process-exited")
                self.assertEqual(tree.close()["ownedPidsRemaining"], 0)
                self.assertTrue(self.identities_gone(records))
            finally:
                tree.close()

    def test_foreign_listener_cannot_satisfy_owned_readiness(self):
        with socket.socket() as foreign, (self.folder / "peer.log").open("wb") as output:
            foreign.bind(("127.0.0.1", 0))
            foreign.listen()
            tree = runtime.OwnedProcess(peer_command(self.records), stdout=output)
            try:
                self.assertTrue(wait_for(lambda: len(read_records(self.records)) == 3))
                with self.assertRaises(runtime.ProbeError) as raised:
                    tree.listeners_owned([foreign.getsockname()[1]])
                self.assertEqual(raised.exception.code, "foreign-listener")
                self.assertEqual(tree.close()["ownedPidsRemaining"], 0)
                with socket.create_connection(foreign.getsockname(), timeout=1):
                    accepted, _ = foreign.accept()
                    accepted.close()
            finally:
                tree.close()

    def test_reaped_worker_cannot_disappear_from_a_later_pss_sample(self):
        with (self.folder / "peer.log").open("wb") as output:
            tree = runtime.OwnedProcess(peer_command(self.records, "--reap-children"), stdout=output)
            try:
                self.assertTrue(wait_for(lambda: len(read_records(self.records)) == 3))
                records = read_records(self.records)
                self.assertEqual(len(tree.sample()["processes"]), 3)
                child = next(p for p in records if p["role"] == "child")
                os.kill(child["pid"], signal.SIGKILL)
                self.assertTrue(wait_for(lambda: self.proc.identity(child["pid"]) is None))
                self.assertTrue(tree.alive(), "controller/root stays alive while a worker disappears")
                with self.assertRaises(runtime.ProbeError):
                    tree.sample()
            finally:
                tree.close()

    def test_controller_signal_cleans_actual_tree(self):
        for grouped, sig in ((grouped, sig) for grouped in (False, True) for sig in (signal.SIGINT, signal.SIGTERM)):
            with self.subTest(signal=sig, grouped=grouped):
                records = self.folder / f"processes-{grouped}-{sig.name}.jsonl"
                reaper = runtime.Subreaper()
                controller = subprocess.Popen([sys.executable, "-B", str(Path(__file__).resolve()),
                    "--runtime", str(args.runtime.resolve()), "--controller", str(sig), "--records", str(records),
                    *(['--group-controller'] if grouped else [])],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                observed = []
                try:
                    self.assertTrue(wait_for(lambda: len(read_records(records)) == (6 if grouped else 3)))
                    observed = read_records(records)
                    controller.send_signal(sig)
                    self.assertEqual(controller.wait(timeout=6), 128 + sig)
                    self.assertEqual(json.loads((self.folder / "cleanup.json").read_text())["ownedPidsRemaining"], 0)
                    self.assertTrue(self.identities_gone(observed))
                finally:
                    if controller.poll() is None:
                        controller.kill()
                        controller.wait(timeout=2)
                    for p in observed:
                        current = self.proc.identity(p["pid"])
                        if current is not None and current.start == p["start"]:
                            try:
                                os.kill(p["pid"], signal.SIGKILL)
                                os.waitpid(p["pid"], 0)
                            except (ProcessLookupError, ChildProcessError):
                                pass
                    reaper.close()

    def test_owned_group_samples_two_roots_without_client_or_foreign_process(self):
        other = self.folder / 'other-root.jsonl'
        with (self.folder / 'forest.log').open('wb') as logs:
            foreign = subprocess.Popen([sys.executable, '-B', '-c', 'import time; time.sleep(30)'])
            group = runtime.OwnedGroup()
            try:
                first = group.spawn(peer_command(self.records), stdout=logs)
                second = group.spawn(peer_command(other), stdout=logs)
                client = group.spawn([sys.executable, '-B', '-c', 'import time; time.sleep(30)'], stdout=logs)
                self.assertTrue(wait_for(lambda: len(read_records(self.records)) == len(read_records(other)) == 3))
                observed = read_records(self.records) + read_records(other)
                sample = group.sample([first, second])
                self.assertEqual({p['pid'] for p in observed}, {p['pid'] for p in sample['processes']})
                self.assertNotIn(client.root.pid, {p['pid'] for p in sample['processes']})
                client.close()
                self.assertIs(runtime.OwnedProcess._group, group)
                self.assertEqual(6, len(group.sample([first, second])['processes']))
                self.assertEqual(0, group.close()['ownedPidsRemaining'])
                self.assertTrue(self.identities_gone(observed))
                self.assertIsNone(foreign.poll(), 'foreign child must survive all owned root cleanups')
            finally:
                group.close()
                foreign.terminate(); foreign.wait(timeout=3)
        self.assertFalse(runtime.OwnedProcess._active)

    def test_owned_group_failed_launch_does_not_restore_reaper_or_lose_roots(self):
        with (self.folder / 'forest.log').open('wb') as logs, runtime.OwnedGroup() as group:
            root = group.spawn(peer_command(self.records), stdout=logs)
            self.assertTrue(wait_for(lambda: len(read_records(self.records)) == 3))
            observed = read_records(self.records)
            with self.assertRaises(FileNotFoundError): group.spawn([str(self.folder / 'missing-program')], stdout=logs)
            self.assertTrue(root.alive())
            self.assertIs(runtime.OwnedProcess._group, group)
            client = group.spawn([sys.executable, '-B', '-c', 'raise SystemExit(7)'], stdout=logs)
            self.assertTrue(wait_for(lambda: not client.alive()))
            client.close()
            self.assertEqual(7, client.process.returncode)
            group.close()
            self.assertTrue(self.identities_gone(observed))
        self.assertFalse(runtime.OwnedProcess._active)

    def test_owned_group_rejects_nested_foreign_duplicate_and_closed_measurement_roots(self):
        with (self.folder / 'forest.log').open('wb') as logs, runtime.OwnedGroup() as group:
            root = group.spawn(peer_command(self.records), stdout=logs)
            self.assertTrue(wait_for(lambda: len(read_records(self.records)) == 3))
            for selected in ([], [root, root], [object()]):
                with self.subTest(selected=selected), self.assertRaises(runtime.ProbeError): group.sample(selected)
            with self.assertRaises(runtime.ProbeError): runtime.OwnedGroup()
            with self.assertRaises(runtime.ProbeError): runtime.OwnedProcess(['unused'], stdout=logs)
            root.close()
            with self.assertRaises(runtime.ProbeError): group.sample([root])

    def test_owned_group_cleanup_failure_still_closes_other_roots_and_stays_failure(self):
        with (self.folder / 'forest.log').open('wb') as logs:
            group = runtime.OwnedGroup()
            first = group.spawn(peer_command(self.records), stdout=logs)
            second = group.spawn([sys.executable, '-B', '-c', 'import time; time.sleep(30)'], stdout=logs)
            try:
                self.assertTrue(wait_for(lambda: len(read_records(self.records)) == 3))
                observed = read_records(self.records)
                with patch.object(first, 'close', side_effect=runtime.ProbeError('injected-cleanup', 'owned fixture failure')):
                    with self.assertRaises(runtime.ProbeError): group.close()
                self.assertTrue(second.closed)
                self.assertTrue(first.alive())
                with self.assertRaises(runtime.ProbeError): group.close()
                self.assertTrue(group.closed)
                self.assertFalse(runtime.OwnedProcess._active)
                self.assertTrue(self.identities_gone(observed))
            finally:
                if not group.closed:
                    try: group.close()
                    except runtime.ProbeError: pass


if __name__ == "__main__":
    unittest.main()

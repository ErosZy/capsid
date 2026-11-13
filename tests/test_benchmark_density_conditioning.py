#!/usr/bin/env python3
"""Conditioning fault controls; synthetic peers never supply performance data."""
import argparse
from contextlib import contextmanager
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--module', type=Path, default=Path(__file__).resolve().parents[1] / 'bench/density_conditioning.py')
args, remaining = parser.parse_known_args()
sys.argv[1:] = remaining
sys.path.insert(0, str(args.module.resolve().parent))
import density_conditioning as conditioning
from density_probe import DensityProbe
from density_control import Coverage
from measurement_runtime import OwnedProcess, ProbeError
assert Path(conditioning.__file__).resolve() == args.module.resolve()


class ConditioningTests(unittest.TestCase):
    def test_cli_uses_the_conditioned_probe_and_preserves_the_original_base(self):
        spec = importlib.util.spec_from_file_location('conditioning_lane_test', args.module.with_name('density-lane.py'))
        lane = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(lane)
        self.assertIs(lane.DensityProbe, conditioning.ConditionedDensityProbe)
        base = DensityProbe(host='unused', worker='unused', workerd='unused', apps='.', out='.', cpuset='0')
        probe = lane.DensityProbe(host='unused', worker='unused', workerd='unused', apps='.', out='.', cpuset='0')
        self.assertEqual(520, base.initial_wave_requests('capsid', 65, 520, 8320))
        self.assertEqual(520, probe.initial_wave_requests('capsid', 65, 520, 8320))
        self.assertEqual(65, probe.initial_wave_requests('workerd', 65, 208000, 8320))
        self.assertEqual(64, probe.parallel)

    def test_metrics_require_one_exact_service_gauge(self):
        raw = b'capsid_channel_http_inflight{scope="service",listener=""} 128\n'
        self.assertEqual(128, conditioning.http_inflight(raw))
        for invalid in (b'', raw + raw, raw.replace(b'service', b'listener'), raw.replace(b'128', b'-1')):
            with self.subTest(raw=invalid), self.assertRaises(ProbeError):
                conditioning.http_inflight(invalid)

    def test_foreign_executable_set_is_rejected_before_signaling(self):
        class Tree:
            root = SimpleNamespace(pid=os.getpid())
            def discover(self):
                return {self.root.pid: self.root}
        with self.assertRaisesRegex(ProbeError, 'exact owned worker set'):
            with conditioning.paused_owned_workers(Tree(), '/not-the-worker', 3):
                self.fail('invalid ownership entered the barrier')

    def exercise_cover_fault(self, fault):
        with tempfile.TemporaryDirectory() as temporary:
            probe = conditioning.ConditionedDensityProbe(host=sys.executable, worker=sys.executable,
                workerd=sys.executable, apps=temporary, out=temporary, cpuset='0', coverage_timeout=5)
            probe.barrier_admin = Path(temporary) / 'admin.sock'
            probe.barrier_log = Path(temporary) / 'barriers.jsonl'
            coverage = Coverage(2, 'fault-control')
            now = [0]
            resumed = []
            waves = []
            output = io.StringIO()
            scope = {'workers': [{'pid': 101, 'start': 1}, {'pid': 102, 'start': 2}],
                     'rootPid': 100, 'rootPaused': False}
            @contextmanager
            def paused(*_):
                try:
                    yield scope
                finally:
                    resumed.append(True)
                    scope.update(pauseSeconds=.1, resumeSignalsIssued=2,
                                 resumeSignalsAttempted=2, resumeFailures=[])
            def wave(ports, nonce, **_):
                waves.append((ports, nonce))
                if fault == 'request-failed':
                    raise ProbeError('request-failed', 'injected request failure')
                if fault == 'expired':
                    now[0] = 6
                selected = ports[:1] if fault == 'short-response' else ports
                return [{'port': port, 'status': 200, 'ok': True, 'bodySHA256': 'a'*64} for port in selected]
            calls = [0]
            def metrics(*_):
                calls[0] += 1
                if calls[0] == 1:
                    count = 1 if fault == 'not-idle' else 0
                elif fault == 'metrics-failed':
                    raise ProbeError('barrier-metrics', 'injected metrics failure')
                else:
                    count = 0 if fault == 'request-failed' else 2
                return f'capsid_channel_http_inflight{{scope="service",listener=""}} {count}\n'.encode()
            def poll():
                if fault == 'missing-worker':
                    coverage.counts = {'one-worker': 2}
                else:
                    coverage.counts = {'one-worker': 1, 'two-worker': 1}
            expected = {'not-idle': 'barrier-not-idle', 'request-failed': 'request-failed',
                        'metrics-failed': 'barrier-metrics', 'short-response': 'barrier-response-count',
                        'missing-worker': 'barrier-worker-coverage', 'expired': 'coverage-deadline'}[fault]
            with patch.object(conditioning, 'paused_owned_workers', side_effect=paused) as pause, \
                 patch.object(conditioning, 'validated_wave', side_effect=wave), \
                 patch.object(conditioning, 'admin_request', side_effect=metrics), \
                 patch.object(conditioning.time, 'monotonic', side_effect=lambda: now[0]), \
                 self.assertRaises(ProbeError) as raised:
                probe.cover(SimpleNamespace(alive=lambda: True), SimpleNamespace(coverage=coverage, poll=poll),
                            'capsid', [12345], 2, 1, 'fault-control', output)
            self.assertEqual(expected, raised.exception.code)
            if fault == 'not-idle':
                pause.assert_not_called()
                self.assertEqual([], waves)
                self.assertEqual([], resumed)
            else:
                self.assertEqual([True], resumed)
                records = [json.loads(line) for line in probe.barrier_log.read_text().splitlines()]
                self.assertEqual(1, len(records))
                self.assertEqual(2, records[0]['resumeSignalsIssued'])
            if fault in ('request-failed', 'metrics-failed', 'short-response', 'not-idle'):
                self.assertEqual('', output.getvalue())

    def test_failed_http_wave_restores_workers(self):
        self.exercise_cover_fault('request-failed')

    def test_failed_admin_observation_restores_workers(self):
        self.exercise_cover_fault('metrics-failed')

    def test_incomplete_response_wave_cannot_supply_success(self):
        self.exercise_cover_fault('short-response')

    def test_one_busy_worker_cannot_replace_complete_pool_coverage(self):
        self.exercise_cover_fault('missing-worker')

    def test_completed_last_wave_cannot_hide_the_original_deadline(self):
        self.exercise_cover_fault('expired')

    def test_unfinished_previous_requests_prevent_a_new_barrier(self):
        self.exercise_cover_fault('not-idle')


@unittest.skipUnless(sys.platform == 'linux', 'owned worker process barriers require Linux /proc')
class OwnedWorkerBarrierTests(unittest.TestCase):
    def exercise(self, *, fail=False, resume_failure=False):
        unrelated = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], start_new_session=True)
        try:
            with tempfile.TemporaryDirectory() as temporary:
                script = Path(temporary) / 'parent.py'
                script.write_text("import subprocess,sys,time\nchildren=[subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']) for _ in range(3)]\ntime.sleep(60)\n")
                with (Path(temporary) / 'logs').open('wb') as logs:
                    tree = OwnedProcess([sys.executable, str(script)], stdout=logs)
                    try:
                        deadline = time.monotonic() + 3
                        while len(tree.discover()) != 4:
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.01)
                        tree.pin_members()
                        original_signal = tree._signal
                        attempted = []
                        failed_identity = []
                        def injected(identity, sig):
                            if sig == signal.SIGCONT:
                                attempted.append(identity.pid)
                                if resume_failure and not failed_identity:
                                    failed_identity.append(identity)
                                    raise PermissionError('injected first resume failure')
                            original_signal(identity, sig)
                        scope = None
                        def run():
                            nonlocal scope
                            with conditioning.paused_owned_workers(tree, sys.executable, 3) as scope:
                                self.assertEqual(3, len(scope['workers']))
                                self.assertTrue(all(tree.proc.identity(p['pid']).state in ('T', 't') for p in scope['workers']))
                                self.assertNotIn(tree.proc.identity(tree.root.pid).state, ('T', 't'))
                                self.assertNotIn(tree.proc.identity(unrelated.pid).state, ('T', 't'))
                                if fail:
                                    raise ValueError('injected wave failure')
                        with patch.object(tree, '_signal', side_effect=injected):
                            if resume_failure:
                                with self.assertRaisesRegex(ProbeError, 'every pinned worker'):
                                    run()
                            elif fail:
                                with self.assertRaisesRegex(ValueError, 'injected wave failure'):
                                    run()
                            else:
                                run()
                        self.assertEqual(3, len(set(attempted)))
                        self.assertEqual(3, scope['resumeSignalsAttempted'])
                        self.assertEqual(2 if resume_failure else 3, scope['resumeSignalsIssued'])
                        self.assertEqual(1 if resume_failure else 0, len(scope['resumeFailures']))
                        for identity in failed_identity:
                            original_signal(identity, signal.SIGCONT)
                        deadline = time.monotonic() + 1
                        while any(p.state in ('T', 't') for p in tree.discover().values()):
                            self.assertLess(time.monotonic(), deadline)
                            time.sleep(.005)
                        self.assertIsNone(unrelated.poll())
                    finally:
                        self.assertEqual(0, tree.close()['ownedPidsRemaining'])
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=3)

    def test_success_resumes_only_the_exact_owned_workers(self):
        self.exercise()

    def test_failed_wave_resumes_workers_and_reaps_the_complete_tree(self):
        self.exercise(fail=True)

    def test_one_failed_resume_does_not_skip_the_remaining_workers(self):
        self.exercise(resume_failure=True)


if __name__ == '__main__':
    unittest.main()

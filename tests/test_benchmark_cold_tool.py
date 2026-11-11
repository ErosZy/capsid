"""Exercise the actual cold C-ABI tool and real worker, including bad apps.

These fixtures are intentionally not performance measurements. The surrounding
owner cleans the complete test tree even when testing an old, broken tool.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--tool', type=Path)
parser.add_argument('--worker', type=Path)
parser.add_argument('--runtime', type=Path, default=Path(__file__).resolve().parents[1] / 'bench/measurement_runtime.py')
args, remaining = parser.parse_known_args()
spec = importlib.util.spec_from_file_location('cold_test_process_owner', args.runtime)
runtime = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runtime
spec.loader.exec_module(runtime)

BODY = '{"status":"ok","total":2039,"count":16}'


@unittest.skipUnless(sys.platform == 'linux', 'cold process-tree PSS is Linux-only')
class ColdToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not args.tool or not args.worker or not args.tool.is_file() or not args.worker.is_file():
            raise RuntimeError('Linux cold tests require actual --tool and --worker binaries')

    def launch(self, root, source, *extra):
        app = root / 'app.mjs'
        app.write_text(source, encoding='utf-8')
        self.out = root / 'stdout.jsonl'
        self.err = root / 'stderr.log'
        stdout = self.out.open('wb')
        stderr = self.err.open('wb')
        self.addCleanup(stdout.close)
        self.addCleanup(stderr.close)
        return runtime.OwnedProcess([str(args.tool), '--worker', str(args.worker),
            '--mode', 'source', '--input', str(app), '--source-name', app.as_uri(),
            '--strict', '--warm', '2', *extra], stdout=stdout, stderr=stderr)

    def finish(self, owned, deadline=20):
        end = time.monotonic() + deadline
        while owned.alive() and time.monotonic() < end:
            owned.discover()
            time.sleep(.01)
        self.assertFalse(owned.alive(), 'tool exceeded bounded test deadline')
        # Inspect before the outer test owner performs any cleanup. In
        # particular it must not hide a child leaked by a cancelled tool.
        descendants = [p for p in owned.discover().values() if p.pid != owned.root.pid]
        self.assertEqual([], descendants, 'C-ABI worker was not reaped by the tool')
        cleanup = owned.close()
        self.assertEqual(0, cleanup['ownedPidsRemaining'])
        return owned.process.returncode, [json.loads(line) for line in self.out.read_text().splitlines()]

    def rejected(self, source, *extra, reason=None):
        with tempfile.TemporaryDirectory(prefix='capsid-cold-reject-') as folder:
            with self.launch(Path(folder), source, *extra) as owned:
                code, rows = self.finish(owned)
                self.assertNotEqual(0, code, self.err.read_text())
                self.assertEqual([], rows, 'failed iteration emitted a successful sample')
                self.assertTrue(self.err.read_text().strip())
                if reason is not None:
                    self.assertRegex(self.err.read_text(), reason)

    def test_success_uses_real_worker_identity_and_complete_counts(self):
        source = 'export default {fetch(){return new Response(' + json.dumps(BODY) + ');}};'
        with tempfile.TemporaryDirectory(prefix='capsid-cold-valid-') as folder:
            with self.launch(Path(folder), source, '--iterations', '2') as owned:
                controller_pid = owned.root.pid
                code, rows = self.finish(owned)
                self.assertEqual(0, code, self.err.read_text())
                self.assertEqual([1, 2], [r['round'] for r in rows])
                for row in rows:
                    self.assertTrue(row['ok'])
                    self.assertNotEqual(controller_pid, row['worker_pid'])
                    self.assertGreater(row['worker_pid'], 0)
                    self.assertEqual(2, row['warm_n'])
                    self.assertEqual(3, row['correctness']['responses_checked'])
                    self.assertEqual(0, row['correctness']['errors'])
                    self.assertEqual([row['pss_kb']] * 2, row['pss_readings_kb'])
                    self.assertGreater(row['pss_kb'], 0)
                    self.assertEqual(row['pss_kb'], row['worker_pss_kb'])
                    members = {(p['pid'], p['start']) for p in row['measurementProcesses']}
                    self.assertIn(row['worker_pid'], {pid for pid, _ in members})
                    self.assertNotIn(controller_pid, {pid for pid, _ in members})
                    for sample in row['pssSamples']:
                        self.assertEqual(members, {(p['pid'], p['start']) for p in sample['processes']})
                        self.assertEqual(sample['pss_kb'], sum(p['pss_kb'] for p in sample['processes']))
                    self.assertGreaterEqual(row['total_ms'], row['ready_ms'])
                    self.assertEqual(row['total_ms'], row['first_ms'])
                    self.assertEqual(2, len(row['warm_latencies_ms']))
                    self.assertAlmostEqual(sum(row['warm_latencies_ms']) / 2, row['warm_ms'], places=4)

    def test_wrong_status_or_body_never_becomes_a_sample(self):
        for status, body in [(500, BODY), (200, 'wrong'), (200, BODY + 'x'), (200, BODY[:-1])]:
            with self.subTest(status=status, body=body):
                self.rejected('export default {fetch(){return new Response(' + json.dumps(body) +
                              ',{status:' + str(status) + '});}};', reason=r'response-(head|body|end):')

    def test_previously_successful_first_request_cannot_mask_a_bad_warm_request(self):
        for bad in ['return new Response("wrong");', 'throw new Error("warm failed");']:
            with self.subTest(bad=bad):
                self.rejected('let n=0; export default {fetch(){if(n++>0){' + bad +
                              '} return new Response(' + json.dumps(BODY) + ');}};', reason=r'(response-body|worker-event):')

    def test_exact_body_accepts_chunked_delivery_and_rejects_wrong_tail(self):
        for tail in [BODY[13:], BODY[13:] + 'x']:
            source = ('export default {fetch(){return new Response(new ReadableStream({start(c){'
                'const e=new TextEncoder(); c.enqueue(e.encode(' + json.dumps(BODY[:13]) + '));'
                'c.enqueue(e.encode(' + json.dumps(tail) + ')); c.close();}}));}};')
            with self.subTest(tail=tail):
                if tail != BODY[13:]:
                    self.rejected(source, reason=r'response-body:')
                else:
                    with tempfile.TemporaryDirectory(prefix='capsid-cold-stream-') as folder:
                        with self.launch(Path(folder), source) as owned:
                            code, rows = self.finish(owned)
                            self.assertEqual(0, code, self.err.read_text())
                            self.assertEqual(1, len(rows))
                            self.assertEqual(3, rows[0]['correctness']['responses_checked'])

    def test_request_timeout_is_fatal_and_reaps_worker(self):
        self.rejected('export default {fetch(){return new Promise(()=>{});}};', '--timeout-ms', '200', reason=r'\btimeout:')

    def test_interrupts_reap_the_actual_worker_without_a_sample(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=sig.name), tempfile.TemporaryDirectory(prefix='capsid-cold-signal-') as folder:
                source = 'export default {fetch(){return new Promise(()=>{});}};'
                with self.launch(Path(folder), source) as owned:
                    deadline = time.monotonic() + 5
                    while len(owned.discover()) < 2 and owned.alive() and time.monotonic() < deadline:
                        time.sleep(.01)
                    self.assertGreaterEqual(len(owned.discover()), 2, self.err.read_text())
                    os.kill(owned.root.pid, sig)
                    code, rows = self.finish(owned, deadline=5)
                    self.assertNotEqual(0, code)
                    self.assertEqual([], rows)
                    self.assertIn('interrupted:', self.err.read_text())

    def test_invalid_counts_are_errors_before_any_iteration(self):
        for value in ('0', '-1', '1junk', '999999999999999999999'):
            with self.subTest(value=value):
                self.rejected('export default {fetch(){return new Response("unused");}};', '--iterations', value,
                              reason=r'(argument-range|number-format):.*iterations')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *remaining])

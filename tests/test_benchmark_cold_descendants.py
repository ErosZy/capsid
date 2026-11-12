"""Actual worker-tree cleanup, witnessed before the outer fixture cleans up."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import signal
import shutil
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--descendant-helper', type=Path, required=True)
parser.add_argument('--descendant-mode', choices=('fork', 'clone'))
options, remaining = parser.parse_known_args()
modes = (options.descendant_mode,) if options.descendant_mode else ('fork', 'clone')
sys.argv[1:] = remaining
spec = importlib.util.spec_from_file_location('original_cold_tool_tests', Path(__file__).with_name('test_benchmark_cold_tool.py'))
original = importlib.util.module_from_spec(spec)
spec.loader.exec_module(original)


@unittest.skipUnless(sys.platform == 'linux', 'actual Linux process adoption')
class DescendantTests(unittest.TestCase):
    # Reuse the existing assertion verbatim: no owned descendant may remain
    # before OwnedProcess.close performs any outer cleanup.
    launch = original.ColdToolTests.launch
    finish = original.ColdToolTests.finish

    @classmethod
    def setUpClass(cls):
        original.ColdToolTests.setUpClass()
        if not options.descendant_helper.is_file():
            raise RuntimeError('actual native descendant helper is required')

    def exercise(self, mode, source, *, interrupt=None, reject=None):
        with tempfile.TemporaryDirectory(prefix='capsid-cold-descendant-') as folder:
            root = Path(folder)
            record = root / 'child.txt'
            actual_worker = original.args.worker.resolve()
            wrapper = root / 'worker-launch'
            shutil.copy2(options.descendant_helper, wrapper)
            Path(str(wrapper) + '.config').write_text(f'{actual_worker}\n{record}\n{mode}\n')
            with patch.object(original.args, 'worker', wrapper):
                with self.launch(root, source, '--timeout-ms', '300' if reject == 'timeout' else '10000') as owned:
                    if interrupt is not None:
                        deadline = time.monotonic() + 5
                        witness = None
                        while time.monotonic() < deadline and owned.alive():
                            tree = owned.discover()
                            if record.is_file():
                                fields = record.read_text().split()
                                if len(fields) == 2:
                                    worker_pid, child_pid = map(int, fields)
                                    try:
                                        executable = Path(os.readlink(f'/proc/{worker_pid}/exe'))
                                    except FileNotFoundError:
                                        executable = None
                                    if executable == actual_worker and child_pid in tree:
                                        witness = tree[child_pid]
                                        self.assertEqual(worker_pid, witness.parent)
                                        break
                            time.sleep(.001)
                        self.assertIsNotNone(witness, 'native worker exec and actual descendant were not observed: ' + self.err.read_text())
                        os.kill(owned.root.pid, interrupt)
                    sampling_wait = max(0, original.args.settle_ms - 1000) / 1000 if interrupt is None and reject is None else 0
                    code, rows = self.finish(owned, deadline=10 + sampling_wait)
                    fields = record.read_text().split()
                    self.assertEqual(2, len(fields))
                    worker_pid, child_pid = map(int, fields)
                    # These checks run after tool exit but before the fixture
                    # directory disappears. finish() already asserted no tree
                    # members before doing its own cleanup.
                    self.assertIsNone(original.runtime.ProcFS().identity(child_pid))
                    if interrupt is not None or reject is not None:
                        self.assertNotEqual(0, code, self.err.read_text())
                        self.assertEqual([], rows)
                        self.assertRegex(self.err.read_text(), 'interrupted:' if interrupt is not None else reject + ':')
                    else:
                        self.assertEqual(0, code, self.err.read_text())
                        self.assertEqual(1, len(rows))
                        row = rows[0]
                        self.assertEqual(worker_pid, row['worker_pid'])
                        self.assertEqual(3, row['correctness']['responses_checked'])
                        self.assertEqual({worker_pid, child_pid}, {p['pid'] for p in row['measurementProcesses']})
                        for sample in row['pssSamples']:
                            self.assertEqual({worker_pid, child_pid}, {p['pid'] for p in sample['processes']})
                            self.assertEqual(sample['pss_kb'], sum(p['pss_kb'] for p in sample['processes']))
                    print(json.dumps(dict(mode=mode, signal=None if interrupt is None else interrupt.name,
                                          rejection=reject, actualWorker=worker_pid, actualChild=child_pid,
                                          beforeOuterCleanupAssertionPassed=True)), flush=True)

    def test_cancel_reaps_both_kinds_of_real_descendant(self):
        for mode in modes:
            for sig in (signal.SIGINT, signal.SIGTERM):
                with self.subTest(mode=mode, signal=sig.name):
                    self.exercise(mode, 'export default {fetch(){return new Promise(()=>{});}};', interrupt=sig)

    def test_normal_sample_includes_descendant_and_reaps_it_before_output(self):
        source = 'export default {fetch(){return new Response(' + json.dumps(original.BODY) + ');}};'
        for mode in modes:
            with self.subTest(mode=mode):
                self.exercise(mode, source)

    def test_wrong_response_reaps_descendant_without_sample(self):
        for mode in modes:
            with self.subTest(mode=mode):
                self.exercise(mode, 'export default {fetch(){return new Response("wrong");}};', reject='response-body')

    def test_timeout_reaps_descendant_without_sample(self):
        for mode in modes:
            with self.subTest(mode=mode):
                self.exercise(mode, 'export default {fetch(){return new Promise(()=>{});}};', reject='timeout')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *original.remaining])

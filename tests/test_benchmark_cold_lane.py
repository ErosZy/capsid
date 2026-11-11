"""Cold producer contracts; fake probes stay inside temporary repositories."""
import argparse
import importlib.util
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
parser.add_argument('--lane', type=Path, default=Path(__file__).resolve().parents[1] / 'bench/cold-start.py')
args, rest = parser.parse_known_args()
sys.argv[1:] = rest
sys.path.insert(0, str(args.lane.resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / 'helpers'))
spec = importlib.util.spec_from_file_location('cold_lane_under_test', args.lane)
lane = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lane)
from cold_measurement_fixture import synthetic_round, compiler_executable, peer_executable
from measurement_runtime import OwnedProcess, ProbeError
from perf_evidence import Archive, STACKS, sha256
from perf_run import machine_identity


def clean_environment():
    return {k: v for k, v in os.environ.items() if k not in ('OUT', 'APPS', 'STACKS', 'SIZES', 'ROUNDS', 'WARM',
        'CAPSID_BENCH_METADATA', 'CAPSID_STRICT', 'CAPSID_MANAGED')}


class PreflightTests(unittest.TestCase):
    def test_missing_metadata_incomplete_protocol_and_nonlinux_never_launch(self):
        with tempfile.TemporaryDirectory(prefix='capsid-cold-preflight-') as folder:
            root = Path(folder)
            marker = root / 'executed'
            executable = root / 'binary'
            executable.write_text('#!' + sys.executable + '\nfrom pathlib import Path\nPath(' + repr(str(marker)) + ').touch()\n')
            executable.chmod(0o700)
            command = [sys.executable, '-B', str(args.lane.resolve()), '--node', str(executable), '--out', str(root / 'out')]
            for extra in ([], ['--metadata', str(root / 'unused'), '--stacks', 'node']):
                result = subprocess.run(command + extra, capture_output=True, text=True, env=clean_environment(), timeout=10)
                self.assertEqual(2, result.returncode)
                self.assertIn('formal cold requires' if sys.platform == 'linux' else 'requires Linux', result.stderr)
                self.assertFalse(marker.exists())
                self.assertFalse((root / 'out').exists())
            if sys.platform == 'linux':
                result = subprocess.run(command + ['--observation', '--stacks', 'node'], capture_output=True, text=True,
                    env=dict(clean_environment(), CAPSID_STRICT='0'), timeout=10)
                self.assertEqual(2, result.returncode)
                self.assertIn('always use strict', result.stderr)
                self.assertFalse(marker.exists())


@unittest.skipUnless(sys.platform == 'linux', 'cold formal producer and owned process CLI tests require Linux')
class ProducerTests(unittest.TestCase):
    def setUp(self):
        affinity = os.sched_getaffinity(0)
        self.cpus = sorted(affinity)[:4]
        self.addCleanup(lambda: self.assertEqual(affinity, os.sched_getaffinity(0), 'lane did not restore caller affinity'))
        self.temp = tempfile.TemporaryDirectory(prefix='capsid-cold-producer-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # Preserve actual file-lock exclusion inside this synthetic fixture;
        # other parallel producer suites and real sessions use other paths.
        lock_directory = patch('perf_run.temp_directory', return_value=str(self.root))
        lock_directory.start()
        self.addCleanup(lock_directory.stop)
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / 'CMakeLists.txt').write_text('project(capsid_runtime VERSION test LANGUAGES C)\n')
        env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
        env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull)
        def git(*values):
            run = subprocess.run(['git', '-C', str(self.source), *values], capture_output=True, text=True, env=env)
            self.assertEqual(0, run.returncode, run.stderr)
            return run.stdout.strip()
        git('init', '--template=')
        git('add', 'CMakeLists.txt')
        git('-c', 'user.name=Capsid synthetic test', '-c', 'user.email=fixture@capsid.invalid',
            '-c', 'commit.gpgsign=false', 'commit', '-m', 'Synthetic cold producer fixture only')
        commit = git('rev-parse', 'HEAD')
        self.origin = {'commit': commit, 'tag': '', 'version': 'test', 'release': 'v-test', 'clean': True}
        self.paths, self.binaries = {}, {}
        for name in ('capsid-worker', 'cold-start', 'capsid-bytecode-compile', 'node', 'deno', 'workerd'):
            file = compiler_executable(self.root) if name == 'capsid-bytecode-compile' else self.root / name
            if name != 'capsid-bytecode-compile': file.write_bytes(('synthetic unused binary ' + name).encode())
            self.paths[name] = file
            self.binaries[name] = {'commit': commit, 'tag': '', 'version': 'test', 'sha256': sha256(file)}
        self.metadata = {'schema': 1, 'session': 'synthetic-cold-session', 'quietSession': True,
            'sourceRoot': str(self.source), 'origin': self.origin, 'machine': machine_identity(), 'binaries': self.binaries}
        self.metadata_file = self.root / 'metadata.json'
        self.metadata_file.write_text(json.dumps(self.metadata))
        self.apps = self.root / 'apps'
        generated = subprocess.run([sys.executable, '-B', str(args.lane.with_name('gen-cold-start-apps.py')), str(self.apps)], capture_output=True, timeout=10)
        self.assertEqual(0, generated.returncode, generated.stderr.decode())
        self.config = SimpleNamespace(repo=self.root, out=self.root / 'bench/results/synthetic',
            apps=self.apps, stacks=list(STACKS['cold-start']), sizes=['10k', '100k', '1m'],
            cpuset=','.join(str(cpu) for cpu in self.cpus),
            rounds=5, warm=50, round_timeout=10, observation=False, metadata=self.metadata_file,
            worker=self.paths['capsid-worker'], cold=self.paths['cold-start'], compiler=self.paths['capsid-bytecode-compile'],
            node=self.paths['node'], deno=self.paths['deno'], workerd=self.paths['workerd'])

    def factory(self, callback=None):
        case = self
        expected = {p.name: p.read_bytes() for p in self.apps.glob('*.mjs')}
        class SyntheticProbe:
            def __init__(self, **options): self.options, self.calls = options, 0
            def __call__(self, *, stack, size, round_no):
                self.calls += 1
                name = 'synthetic-probe-' + str(self.calls)
                kind = 'capsid' if stack.startswith('capsid-') else stack
                source = self.options['apps'] / f'app-{kind}-{size}.mjs'
                case.assertEqual(expected[source.name], source.read_bytes(), 'shared APPS mutation reached the active fixture')
                file = self.options['out'] / name / 'runtime/bundle.mjs'
                file.parent.mkdir(parents=True)
                file.write_bytes(source.read_bytes())
                result = dict(synthetic_round(stack, size, round_no, warm=self.options['warm_requests']), probeDirectory=name)
                if callback is not None: callback(self, result)
                return result
        return SyntheticProbe

    def test_full_synthetic_archive_runs_compiler_metadata_seal_and_real_validator(self):
        calls = []
        def changed_shared_apps(adapter, result):
            calls.append(result)
            if adapter.calls == 1: (self.apps / 'app-node-10k.mjs').write_text('shared APPS changed after freezing')
        with patch.object(lane, 'ColdProbe', self.factory(changed_shared_apps)):
            result = lane.run_lane(self.config)
        self.assertTrue(result['publicationEligible'], 'temporary synthetic contract fixture only')
        self.assertEqual(15, result['cells'])
        self.assertEqual(90, len(calls))
        archive = Archive(self.config.out, self.root)
        self.assertEqual(15, len(archive.manifest['records']))
        self.assertEqual('python-http-exact-v1', archive.manifest['protocol']['httpClient'])
        self.assertEqual(self.cpus, archive.manifest['clientAffinity'])
        for record in archive.manifest['records']:
            self.assertEqual(1, len(record['discardedSamples']))
            self.assertEqual(1, len(record['discardedCorrectness']))
        self.assertEqual(3, len(list((self.config.out / 'compiled').glob('*/bundle.qjsb'))))
        self.assertTrue(all(sha256(self.config.out / n) == value for n, value in archive.manifest['runner']['files'].items()))
        self.assertFalse((self.root / 'data.js').exists())
        archive.recheck()

    def test_bad_identity_source_machine_and_fixture_fail_before_launch(self):
        for field in ('binary', 'cold-source', 'machine', 'source', 'fixture'):
            with self.subTest(field=field):
                data = json.loads(json.dumps(self.metadata))
                dirty = self.source / 'dirty'
                file = self.apps / 'app-node-10k.mjs'
                original = file.read_bytes()
                if field == 'binary': data['binaries']['workerd']['sha256'] = '0' * 64
                if field == 'cold-source': data['binaries']['cold-start']['commit'] = 'f' * 40
                if field == 'machine': data['machine']['kernel'] = 'wrong kernel'
                if field == 'source': dirty.touch()
                if field == 'fixture': file.write_bytes(b'changed fixture')
                self.metadata_file.write_text(json.dumps(data))
                try:
                    with patch.object(lane, 'ColdProbe') as adapter, patch.object(lane, 'compile_fixture') as compiler:
                        with self.assertRaises(ProbeError): lane.run_lane(self.config)
                        adapter.assert_not_called()
                        compiler.assert_not_called()
                    self.assertFalse(self.config.out.exists())
                finally:
                    file.write_bytes(original)
                    dirty.unlink(missing_ok=True)
                    self.metadata_file.write_text(json.dumps(self.metadata))

    def test_discarded_failure_warm_failure_and_reused_process_publish_no_summary(self):
        for mode in ('discard', 'warm', 'reused'):
            with self.subTest(mode=mode):
                self.config.out = self.root / 'bench/results' / mode
                def fail(adapter, result):
                    if mode == 'discard' and adapter.calls == 1: raise ProbeError('cold-response', 'discarded response failed')
                    if mode == 'warm' and adapter.calls == 2: result['correctness']['errors'] = 1
                    if mode == 'reused' and adapter.calls == 2:
                        prior = synthetic_round(result['stack'], result['size'], 0)
                        for key in ('measurementRoot', 'measurementProcesses', 'pssSamples', 'worker_pid'):
                            if key in prior: result[key] = prior[key]
                with patch.object(lane, 'ColdProbe', self.factory(fail)):
                    with self.assertRaises(ProbeError): lane.run_lane(self.config)
                self.assertFalse((self.config.out / 'cells.txt').exists())
                self.assertFalse((self.config.out / 'lane.json').exists())

    def test_changed_binary_or_frozen_input_is_rejected_before_summary(self):
        for mode in ('binary', 'fixture'):
            with self.subTest(mode=mode):
                self.config.out = self.root / 'bench/results' / mode
                original = self.paths['workerd'].read_bytes()
                def change(adapter, result):
                    if adapter.calls == 90:
                        target = self.paths['workerd'] if mode == 'binary' else adapter.options['apps'] / 'app-node-10k.mjs'
                        target.write_bytes(b'changed during run')
                try:
                    with patch.object(lane, 'ColdProbe', self.factory(change)):
                        with self.assertRaises(ProbeError): lane.run_lane(self.config)
                    self.assertFalse((self.config.out / 'cells.txt').exists())
                    self.assertFalse((self.config.out / 'lane.json').exists())
                finally: self.paths['workerd'].write_bytes(original)

    def test_output_creation_race_preserves_other_run(self):
        self.config.observation = True
        self.config.out.mkdir(parents=True)
        marker = self.config.out / 'other-run'
        marker.write_bytes(b'preserve other evidence')
        handlers = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
        try:
            with patch.object(lane, 'parse_args', return_value=self.config), self.assertRaises(FileExistsError): lane.main([])
            self.assertEqual(['other-run'], [p.name for p in self.config.out.iterdir()])
            self.assertEqual(b'preserve other evidence', marker.read_bytes())
        finally:
            for number, handler in handlers.items(): signal.signal(number, handler)

    def cli(self, output, peer):
        return [sys.executable, '-B', str(args.lane.resolve()), '--observation', '--out', str(output),
                '--apps', str(self.apps), '--node', str(peer), '--cpuset', self.config.cpuset,
                '--stacks', 'node', '--sizes', '10k', '--rounds', '1', '--warm', '2']

    def test_concurrent_public_controllers_own_distinct_files_and_ports(self):
        peer = peer_executable(self.root)
        helper = Path(__file__).resolve().parent / 'helpers/cold_measurement_fixture.py'
        processes = []
        original = {p.name: sha256(p) for p in self.apps.iterdir()}
        for index in range(2):
            output = self.root / ('cli-' + str(index))
            harness = [sys.executable, '-B', str(helper), '--controller', '--stdout', str(self.root / f'cli-{index}.stdout'),
                '--stderr', str(self.root / f'cli-{index}.stderr'), '--report', str(self.root / f'cli-{index}.harness.json'),
                '--', *self.cli(output, peer)]
            processes.append((output, subprocess.Popen(harness, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=clean_environment())))
        ports = []
        for output, process in processes:
            stdout, stderr = process.communicate(timeout=60)
            self.assertEqual(0, process.returncode, stderr.decode())
            rows = [json.loads(line) for line in (output / 'probes.jsonl').read_text().splitlines()]
            self.assertEqual(2, len(rows))
            self.assertTrue(all(row['cleanup']['ownedPidsRemaining'] == 0 for row in rows))
            ports += [json.loads(p.read_text())['ports'][0] for p in output.glob('probe-*/launch.json')]
            self.assertFalse((output / 'lane.json').exists())
        self.assertEqual(4, len(set(ports)))
        self.assertEqual(original, {p.name: sha256(p) for p in self.apps.iterdir()})
        output = processes[0][0]
        saved = {p.relative_to(output).as_posix(): sha256(p) for p in output.rglob('*') if p.is_file()}
        reused = subprocess.run(self.cli(output, peer), capture_output=True, env=clean_environment(), timeout=10)
        self.assertEqual(2, reused.returncode)
        self.assertEqual(saved, {p.relative_to(output).as_posix(): sha256(p) for p in output.rglob('*') if p.is_file()})

    def test_public_controller_interrupts_leave_no_children_or_cells(self):
        peer = peer_executable(self.root)
        for number in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=number.name):
                output = self.root / number.name
                with (self.root / (number.name + '.stdout')).open('xb') as stdout, (self.root / (number.name + '.stderr')).open('xb') as stderr:
                    with OwnedProcess(self.cli(output, peer), stdout=stdout, stderr=stderr, env=clean_environment()) as owned:
                        deadline = time.monotonic() + 10
                        while owned.alive() and len(owned.discover()) < 4 and time.monotonic() < deadline: time.sleep(.005)
                        self.assertGreaterEqual(len(owned.discover()), 4)
                        os.kill(owned.root.pid, number)
                        while owned.alive() and time.monotonic() < deadline: time.sleep(.005)
                        self.assertFalse(owned.alive())
                        self.assertFalse([p for p in owned.discover().values() if p.pid != owned.root.pid],
                                         'interrupted controller leaked before outer test cleanup')
                        owned.close()
                        self.assertNotEqual(0, owned.process.returncode)
                self.assertEqual('interrupted', json.loads((output / 'failed.json').read_text())['error']['code'])
                self.assertFalse((output / 'cells.txt').exists())
                self.assertFalse((output / 'lane.json').exists())


if __name__ == '__main__': unittest.main()

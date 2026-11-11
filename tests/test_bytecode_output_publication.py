"""Actual compiler output transactions in owned native/mounted directories."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--scratch-root', type=Path)
args, remaining = parser.parse_known_args()
sys.argv[1:] = remaining


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.base = (args.scratch_root or Path(tempfile.gettempdir())).resolve()
        self.assertTrue(self.base.is_dir())
        self.temp = tempfile.TemporaryDirectory(prefix='capsid output tests-', dir=self.base)
        self.root = Path(self.temp.name).resolve()
        def cleanup():
            self.assertEqual(self.base, self.root.parent)
            self.assertTrue(self.root.name.startswith('capsid output tests-'))
            self.temp.cleanup()
        self.addCleanup(cleanup)
        self.source = self.root / 'input module.mjs'
        self.source.write_bytes(b'export default {fetch(){return new Response("ok");}};\n')
        self.outputs = [self.root / name for name in ('bundle.qjsb', 'attestation.json', 'message.bin')]

    def command(self, outputs=None, source=None):
        return [str(args.compiler.resolve()), '--source', str(source or self.source), '--source-name', 'file:///app/entry.mjs',
                '--application', 'compiler-control', '--version', 'v1', '--key-id', 'test-key',
                *[item for flag, file in zip(('--bytecode-out', '--attestation-out', '--signing-message-out'), outputs or self.outputs)
                  for item in (flag, str(file))]]

    def run_compiler(self, outputs=None, source=None):
        return subprocess.run(self.command(outputs, source), capture_output=True, timeout=30)

    def assert_no_temporaries(self):
        self.assertFalse(list(self.root.rglob('*.tmp.*')), 'compiler left an invocation temporary')

    def assert_diagnostic(self, result):
        self.assertEqual(1, result.returncode, result.stderr.decode(errors='replace'))
        self.assertIn(b'output commit failed', result.stderr)
        self.assertRegex(result.stderr, rb'(errno|win32)=[1-9][0-9]*', 'missing operating-system failure code')
        self.assertNotIn(b'failed ()', result.stderr)

    def verify_outputs(self, outputs=None, source=None):
        bytecode, attestation, message = outputs or self.outputs
        source = source or self.source
        claims = json.loads(attestation.read_bytes())
        self.assertEqual('sha256:' + hashlib.sha256(source.read_bytes()).hexdigest(), claims['sourceSha256'])
        self.assertEqual('sha256:' + hashlib.sha256(bytecode.read_bytes()).hexdigest(), claims['bytecodeSha256'])
        fields = ('schema', 'application', 'version', 'sourceName', 'sourceSha256', 'bytecodeSha256', 'compatibilityId', 'keyId')
        expected = b'capsid-bytecode-attestation-v1\0'
        for field in fields:
            value = claims[field].encode('utf-8')
            expected += struct.pack('>I', len(value)) + value
        self.assertEqual(expected, message.read_bytes())
        self.assert_no_temporaries()

    def test_fresh_outputs_in_paths_with_spaces_are_complete_and_deterministic(self):
        result = self.run_compiler()
        self.assertEqual(0, result.returncode, result.stderr.decode(errors='replace'))
        self.verify_outputs()
        second = self.root / 'second outputs'
        second.mkdir()
        outputs = [second / p.name for p in self.outputs]
        result = self.run_compiler(outputs)
        self.assertEqual(0, result.returncode, result.stderr.decode(errors='replace'))
        self.verify_outputs(outputs)
        self.assertEqual([p.read_bytes() for p in self.outputs], [p.read_bytes() for p in outputs])

    def test_each_preexisting_final_survives_and_partial_outputs_are_removed(self):
        for index in range(3):
            with self.subTest(index=index):
                sentinel = b'existing output belongs to an earlier invocation'
                self.outputs[index].write_bytes(sentinel)
                result = self.run_compiler()
                self.assert_diagnostic(result)
                self.assertEqual(sentinel, self.outputs[index].read_bytes())
                self.assertFalse([p for i, p in enumerate(self.outputs) if i != index and p.exists()])
                self.assert_no_temporaries()
                self.outputs[index].unlink()

    def test_existing_directory_is_preserved_at_every_output_position(self):
        for index in range(3):
            with self.subTest(index=index):
                self.outputs[index].mkdir()
                result = self.run_compiler()
                self.assert_diagnostic(result)
                self.assertTrue(self.outputs[index].is_dir())
                self.assertEqual([], list(self.outputs[index].iterdir()))
                self.assertFalse([p for i, p in enumerate(self.outputs) if i != index and p.exists()])
                self.assert_no_temporaries()
                self.outputs[index].rmdir()

    def test_missing_or_nondirectory_later_parent_removes_earlier_temporary(self):
        for exists in (False, True):
            with self.subTest(parent_is_file=exists):
                parent = self.root / 'unavailable parent'
                if exists: parent.write_bytes(b'preserve parent file')
                outputs = [self.outputs[0], parent / 'attestation.json', self.outputs[2]]
                result = self.run_compiler(outputs)
                self.assert_diagnostic(result)
                self.assertFalse(self.outputs[0].exists())
                self.assertFalse(self.outputs[2].exists())
                self.assert_no_temporaries()
                if exists:
                    self.assertEqual(b'preserve parent file', parent.read_bytes())
                    parent.unlink()

    def test_two_compilers_cannot_mix_or_replace_the_winning_transaction(self):
        other = self.root / 'other source.mjs'
        other.write_bytes(b'export default {fetch(){return new Response("other");}};\n')
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(self.run_compiler, None, source) for source in (self.source, other)]
            results = [f.result() for f in futures]
        self.assertEqual([0, 1], sorted(result.returncode for result in results), [r.stderr.decode(errors='replace') for r in results])
        winner = (self.source, other)[next(i for i, result in enumerate(results) if result.returncode == 0)]
        self.verify_outputs(source=winner)
        original = [p.read_bytes() for p in self.outputs]
        result = self.run_compiler(source=other if winner == self.source else self.source)
        self.assert_diagnostic(result)
        self.assertEqual(original, [p.read_bytes() for p in self.outputs])

    def test_lexically_different_aliases_are_not_published_twice(self):
        outputs = [self.outputs[0], str(self.root) + '/./bundle.qjsb', self.outputs[2]]
        result = self.run_compiler(outputs)
        self.assert_diagnostic(result)
        self.assertFalse(self.outputs[0].exists())
        self.assertFalse(self.outputs[2].exists())
        self.assert_no_temporaries()

    def test_preexisting_temporary_is_preserved_when_exclusive_creation_fails(self):
        sentinel = b'temporary file predates this invocation'
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            class Startup(ctypes.Structure):
                _fields_ = [('cb', wintypes.DWORD), ('reserved', wintypes.LPWSTR), ('desktop', wintypes.LPWSTR),
                    ('title', wintypes.LPWSTR), ('x', wintypes.DWORD), ('y', wintypes.DWORD),
                    ('width', wintypes.DWORD), ('height', wintypes.DWORD), ('chars_x', wintypes.DWORD),
                    ('chars_y', wintypes.DWORD), ('fill', wintypes.DWORD), ('flags', wintypes.DWORD),
                    ('show', wintypes.WORD), ('reserved_size', wintypes.WORD), ('reserved_bytes', ctypes.c_void_p),
                    ('stdin', wintypes.HANDLE), ('stdout', wintypes.HANDLE), ('stderr', wintypes.HANDLE)]
            class Process(ctypes.Structure):
                _fields_ = [('handle', wintypes.HANDLE), ('thread', wintypes.HANDLE),
                            ('pid', wintypes.DWORD), ('tid', wintypes.DWORD)]
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.POINTER(Startup), ctypes.POINTER(Process)]
            kernel.CreateProcessW.restype = wintypes.BOOL
            kernel.ResumeThread.argtypes = [wintypes.HANDLE]
            kernel.ResumeThread.restype = wintypes.DWORD
            kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            kernel.WaitForSingleObject.restype = wintypes.DWORD
            kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
            kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            startup, process = Startup(), Process()
            startup.cb = ctypes.sizeof(startup)
            command = ctypes.create_unicode_buffer(subprocess.list2cmdline(self.command()))
            self.assertTrue(kernel.CreateProcessW(str(args.compiler.resolve()), command, None, None, False,
                0x00000004 | 0x08000000, None, None, ctypes.byref(startup), ctypes.byref(process)), ctypes.get_last_error())
            try:
                temporary = Path(str(self.outputs[0]) + '.tmp.' + str(process.pid))
                temporary.write_bytes(sentinel)
                self.assertNotEqual(0xFFFFFFFF, kernel.ResumeThread(process.thread))
                self.assertEqual(0, kernel.WaitForSingleObject(process.handle, 30000))
                code = wintypes.DWORD()
                self.assertTrue(kernel.GetExitCodeProcess(process.handle, ctypes.byref(code)))
                self.assertEqual(1, code.value)
            finally:
                if kernel.WaitForSingleObject(process.handle, 0) != 0:
                    kernel.TerminateProcess(process.handle, 1)
                    kernel.WaitForSingleObject(process.handle, 5000)
                kernel.CloseHandle(process.thread)
                kernel.CloseHandle(process.handle)
        else:
            pid = os.fork()
            if pid == 0:
                try:
                    temporary = Path(str(self.outputs[0]) + '.tmp.' + str(os.getpid()))
                    temporary.write_bytes(sentinel)
                    with (self.root / 'child.stderr').open('wb') as error:
                        os.dup2(error.fileno(), 2)
                        os.execv(str(args.compiler.resolve()), self.command())
                except BaseException:
                    os._exit(127)
            _, status = os.waitpid(pid, 0)
            self.assertTrue(os.WIFEXITED(status))
            self.assertEqual(1, os.WEXITSTATUS(status))
            temporary = Path(str(self.outputs[0]) + '.tmp.' + str(pid))
        self.assertTrue(temporary.is_file(), 'compiler removed a temporary it did not create')
        self.assertEqual(sentinel, temporary.read_bytes())
        self.assertFalse(any(p.exists() for p in self.outputs))
        self.assertEqual([temporary], list(self.root.rglob('*.tmp.*')))

    @unittest.skipUnless(sys.platform == 'linux', 'POSIX final-symlink primitive is tested on Linux')
    def test_existing_symlink_and_its_owned_target_are_preserved(self):
        sentinel = self.root / 'sentinel'
        sentinel.write_bytes(b'owned symlink target stays unchanged')
        self.outputs[1].symlink_to(sentinel)
        result = self.run_compiler()
        self.assert_diagnostic(result)
        self.assertTrue(self.outputs[1].is_symlink())
        self.assertEqual(b'owned symlink target stays unchanged', sentinel.read_bytes())
        self.assertFalse(self.outputs[0].exists())
        self.assertFalse(self.outputs[2].exists())
        self.assert_no_temporaries()


if __name__ == '__main__': unittest.main()

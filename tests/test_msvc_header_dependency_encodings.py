"""Exercise real MSVC header dependencies in private native and UTF-8 consoles."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    for name in ['cmake', 'prefix-module', 'fixture-script', 'test-root',
                 'c-compiler', 'cxx-compiler', 'ninja']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--child-code-page', type=int)
    parser.add_argument('--child-input-code-page', type=int)
    parser.add_argument('--fixture-test-root', type=Path)
    args = parser.parse_args()
    if os.name != 'nt':
        raise RuntimeError('MSVC console encoding checks require Windows')
    root = Path(args.test_root)
    root.mkdir(parents=True, exist_ok=True)
    if args.child_code_page is not None:
        code_page = args.child_code_page
        kernel = ctypes.windll.kernel32
        input_code_page = args.child_input_code_page or code_page
        # CMake's initial compiler-ABI detection runs before our module and
        # cannot generate its own Ninja rules in the reverse mixed console.
        # Detect the real compiler first, then exercise our module with that
        # existing cache under the requested mixed code pages.
        reverse_mixed = input_code_page == 65001 and code_page != 65001
        fixture_input_code_page = code_page if reverse_mixed else input_code_page
        assert kernel.SetConsoleCP(fixture_input_code_page)
        assert kernel.SetConsoleOutputCP(code_page)
        assert kernel.GetConsoleCP() == fixture_input_code_page
        assert kernel.GetConsoleOutputCP() == code_page
        fixture_root = args.fixture_test_root or root / 'products'
        command = [args.cmake, '-DCAPSID_PREFIX_MODULE=' + args.prefix_module,
                   '-DCAPSID_PREFIX_TEST_ROOT=' + str(fixture_root),
                   '-DCAPSID_PREFIX_C_COMPILER=' + args.c_compiler,
                   '-DCAPSID_PREFIX_CXX_COMPILER=' + args.cxx_compiler,
                   '-DCAPSID_PREFIX_NINJA=' + args.ninja, '-P', args.fixture_script]
        env = dict(os.environ, TEMP=str(root), TMP=str(root))
        started = time.monotonic()
        with (root / 'fixture.stdout.log').open('xb') as out, (root / 'fixture.stderr.log').open('xb') as err:
            process = subprocess.Popen(command, env=env, stdout=out, stderr=err)
            try:
                code = process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill.exe', '/PID', str(process.pid), '/T', '/F'],
                               stdout=err, stderr=err, timeout=10)
                raise
            reverse_checks = []
            if code == 0 and reverse_mixed:
                assert kernel.SetConsoleCP(input_code_page)
                deadline = started + 60

                def phase_step(arguments):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('reverse mixed regression exceeded the original fixture budget')
                    child = subprocess.Popen(arguments, env=env, stdout=out, stderr=err)
                    try:
                        result = child.wait(timeout=remaining)
                    except subprocess.TimeoutExpired:
                        subprocess.run(['taskkill.exe', '/PID', str(child.pid), '/T', '/F'],
                                       stdout=err, stderr=err, timeout=10)
                        raise
                    if result != 0:
                        raise RuntimeError('reverse mixed regression command failed: ' + repr(arguments))

                runs = list(fixture_root.iterdir())
                assert len(runs) == 1
                for variant, values in [('ON', (53, 67)), ('MIGRATE', (61, 71))]:
                    source = runs[0] / variant / 'source'
                    build = runs[0] / variant / 'build'
                    objects = [build / 'CMakeFiles/prefix-c.dir/main.c.obj',
                               build / 'CMakeFiles/prefix-cxx.dir/main.cc.obj']
                    before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in objects]
                    phase_step([args.cmake, '-S', str(source), '-B', str(build), '-DCAPSID_PREFIX_FIX=ON'])
                    assert [hashlib.sha256(p.read_bytes()).hexdigest() for p in objects] == before
                    for value in values:
                        (source / 'value.h').write_text('#define CAPSID_PREFIX_VALUE ' + str(value) + '\n', encoding='utf-8')
                        phase_step([args.cmake, '--build', str(build), '--parallel', '2'])
                        for target in ['prefix-c', 'prefix-cxx']:
                            actual = subprocess.check_output([str(build / (target + '.exe'))],
                                env=env, timeout=max(0.1, deadline-time.monotonic())).decode().strip()
                            assert actual == str(value), (variant, target, value, actual)
                            reverse_checks.append(dict(variant=variant, target=target, expected=value, actual=int(actual)))
                        deps = subprocess.check_output([args.ninja, '-C', str(build), '-t', 'deps'],
                            env=env, timeout=max(0.1, deadline-time.monotonic())).decode('utf-8', errors='replace')
                        for target in ['prefix-c', 'prefix-cxx']:
                            assert re.search(re.escape(target) + r'[.]dir/[^\r\n]+[.]obj: #deps [1-9][0-9]*,', deps)
                assert len(reverse_checks) == 8 and time.monotonic() <= deadline
        report = dict(code_page=code_page, observed_code_page=kernel.GetConsoleOutputCP(),
                      input_code_page=input_code_page, observed_input_code_page=kernel.GetConsoleCP(),
                      fresh_compiler_detection_input_code_page=fixture_input_code_page,
                      reverse_mixed_incremental_checks_passed=len(reverse_checks),
                      reverse_mixed_reconfigure_kept_existing_objects=bool(reverse_checks),
                      reverse_mixed_incremental_checks=reverse_checks,
                      ansi_code_page=kernel.GetACP(), exit_code=code,
                      seconds=time.monotonic() - started, original_fixture_budget_seconds=60,
                      fixture_test_root=str(fixture_root),
                      original_fixture_sha256=hashlib.sha256(Path(args.fixture_script).read_bytes()).hexdigest())
        (root / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        if code != 0:
            raise RuntimeError('MSVC dependency fixture failed; see ' + str(root))
        return
    run = Path(tempfile.mkdtemp(prefix='run-', dir=root))
    # Keep CMake's nested compiler checks below the MSVC object path limit.
    # Logs and the result index remain under the requested evidence root.
    scratch_base = Path(root.resolve().anchor) / 'capsid-verification-tmp'
    scratch_base.mkdir(parents=True, exist_ok=True)
    fixture_run = Path(tempfile.mkdtemp(prefix='hd-', dir=scratch_base))
    reports = []
    legacy_code_page = ctypes.windll.kernel32.GetACP()
    if legacy_code_page == 65001:
        legacy_code_page = ctypes.windll.kernel32.GetOEMCP()
    if legacy_code_page == 65001:
        legacy_code_page = 1252
    for name, input_code_page, code_page in [
        ('native', ctypes.windll.kernel32.GetACP(), ctypes.windll.kernel32.GetACP()),
        ('utf8', 65001, 65001),
        ('mixed-native-input', legacy_code_page, 65001),
        ('mixed-utf8-input', 65001, legacy_code_page),
    ]:
        directory = run / name
        directory.mkdir()
        command = [sys.executable, '-X', 'utf8', str(Path(__file__).resolve())]
        for option in ['cmake', 'prefix_module', 'fixture_script', 'c_compiler', 'cxx_compiler', 'ninja']:
            command += ['--' + option.replace('_', '-'), getattr(args, option)]
        command += ['--test-root', str(directory), '--child-code-page', str(code_page),
                    '--child-input-code-page', str(input_code_page),
                    '--fixture-test-root', str(fixture_run / name / 'products')]
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        with (directory / 'launcher.stdout.log').open('xb') as out, (directory / 'launcher.stderr.log').open('xb') as err:
            child = subprocess.Popen(command, stdout=out, stderr=err, startupinfo=startup,
                                     creationflags=subprocess.CREATE_NEW_CONSOLE)
            try:
                code = child.wait(timeout=70)
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill.exe', '/PID', str(child.pid), '/T', '/F'],
                               stdout=err, stderr=err, timeout=10)
                raise
        if code != 0:
            raise RuntimeError(name + ' console dependency checks failed; see ' + str(directory))
        report = json.loads((directory / 'result.json').read_text(encoding='utf-8'))
        assert report['exit_code'] == 0 and report['observed_code_page'] == code_page
        assert report['observed_input_code_page'] == input_code_page
        reports.append(report)
    (run / 'results.json').write_text(json.dumps(reports, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, console_modes=reports, evidence=str(run))))


if __name__ == '__main__':
    main()

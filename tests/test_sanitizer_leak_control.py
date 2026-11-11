import argparse
import os
import re
import resource
import signal
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--case', choices=['clean', 'leak'], required=True)
args = parser.parse_args()
asan = os.environ.get('ASAN_OPTIONS', '')
assert 'detect_leaks=1' in asan and 'detect_leaks=0' not in asan, asan
assert 'halt_on_error=1' in os.environ.get('UBSAN_OPTIONS', ''), 'UBSan recovery policy missing'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
process = subprocess.Popen([args.binary, args.case], stdout=subprocess.PIPE,
    stderr=subprocess.PIPE, text=True, start_new_session=True)
try:
    stdout, stderr = process.communicate(timeout=30)
except subprocess.TimeoutExpired:
    os.killpg(process.pid, signal.SIGKILL)
    stdout, stderr = process.communicate()
    print(stdout, end=''); print(stderr, end='')
    raise AssertionError('sandbox leak control timed out')
print(stdout, end=''); print(stderr, end='')
proof = re.search(r'SANITIZER_LEAK_CONTROL features=(\d+) landlock=(\d+) seccomp=(\d+) leak=(\d+)', stderr)
assert proof and int(proof[2]) > 0 and proof[3] == '2', 'full sandbox proof missing'
assert 'LeakSanitizer has encountered a fatal error' not in stderr
if args.case == 'clean':
    assert process.returncode == 0, process.returncode
    assert 'ERROR:' not in stderr and 'runtime error:' not in stderr
else:
    assert process.returncode != 0
    assert 'ERROR: LeakSanitizer: detected memory leaks' in stderr
    assert '128 byte(s) leaked in 1 allocation(s)' in stderr
print('PASS: explicit sanitizer sandbox ' + args.case)

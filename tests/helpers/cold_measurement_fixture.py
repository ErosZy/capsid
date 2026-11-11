"""Synthetic cold records for temporary contract tests only, never run data."""


def synthetic_round(stack, size, round_no, warm=50):
    root = {"pid": 200 + round_no, "start": 10000 + round_no}
    sample = lambda: {"pss_kb": 1024, "processes": [dict(root, pss_kb=1024)]}
    row = {"stack": stack, "size": size, "round": round_no, "ok": True,
           "warm_n": warm, "total_ms": 100, "ready_ms": 80, "warm_ms": 1,
           "warm_latencies_ms": [1] * warm, "pss_kb": 1024,
           "pss_readings_kb": [1024, 1024], "pssSamples": [sample(), sample()],
           "measurementRoot": dict(root), "measurementProcesses": [dict(root)],
           "probeProcess": {"pid": 10, "start": 100},
           "transport": "c-abi" if stack.startswith("capsid-") else "http",
           "cleanup": {"ownedPidsRemaining": 0},
           "correctness": {"ok": True, "errors_as_success": False, "responses_checked": warm + 1,
                           "errors": 0, "timeouts": 0, "mismatches": 0}}
    if stack.startswith("capsid-"):
        row.update(worker_pid=root["pid"], posture="c-abi-strict", sourceProbeRound=1)
    return row


def peer_executable(folder, mode="ok"):
    from pathlib import Path
    import sys
    target = Path(folder) / ("node-peer-" + mode)
    target.write_text("#!" + sys.executable + "\nimport os,runpy,sys\n" +
        "os.environ['CAPSID_TEST_COLD_PEER']=" + repr(mode) + "\n" +
        "sys.argv=[" + repr(str(Path(__file__).resolve())) + ",'--node',*sys.argv[1:]]\n" +
        "runpy.run_path(sys.argv[0],run_name='__main__')\n", encoding="utf-8")
    target.chmod(0o700)
    return target


def compiler_executable(folder, bad=False):
    from pathlib import Path
    import sys
    target = Path(folder) / ("compiler-bad" if bad else "compiler-fixture")
    code = '''import hashlib,json,sys
from pathlib import Path
args=dict(zip(sys.argv[1::2],sys.argv[2::2]))
if BAD: sys.exit(3)
data=b'synthetic bytecode test fixture'
Path(args['--bytecode-out']).write_bytes(data)
att={'sourceName':args['--source-name'], 'sourceSha256':'sha256:'+hashlib.sha256(Path(args['--source']).read_bytes()).hexdigest(),
     'bytecodeSha256':'sha256:'+hashlib.sha256(data).hexdigest()}
Path(args['--attestation-out']).write_text(json.dumps(att))
Path(args['--signing-message-out']).write_bytes(b'synthetic compiler message')
'''.replace('BAD', repr(bad))
    target.write_text("#!" + sys.executable + "\n" + code, encoding="utf-8")
    target.chmod(0o700)
    return target


def run_peer(app):
    import http.server
    import os
    import re
    import time
    port = int(re.search(r'server.listen\((\d+),', app.read_text())[1])
    mode = os.environ.get("CAPSID_TEST_COLD_PEER", "ok")
    child = os.fork()
    if child == 0:
        os.fork()
        while True: time.sleep(1)
    class Handler(http.server.BaseHTTPRequestHandler):
        count = 0
        def do_GET(self):
            Handler.count += 1
            if mode == "exit-warm" and Handler.count > 1: os._exit(3)
            wrong = mode == "bad-first" or mode == "bad-warm" and Handler.count > 1
            body = b'wrong' if wrong else b'{"status":"ok","total":2039,"count":16}'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *_): pass
    server = http.server.HTTPServer(('127.0.0.1', port), Handler)
    print('READY', flush=True)
    server.serve_forever(poll_interval=.01)


def run_controller(argv, stdout, stderr, report):
    from pathlib import Path
    import json
    import sys
    import time
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'bench'))
    from measurement_runtime import OwnedProcess
    with stdout.open('xb') as out, stderr.open('xb') as err:
        with OwnedProcess(argv, stdout=out, stderr=err) as owned:
            deadline = time.monotonic() + 45
            while owned.alive() and time.monotonic() < deadline:
                owned.discover()
                time.sleep(.01)
            assert not owned.alive(), 'test controller deadline'
            assert not [p for p in owned.discover().values() if p.pid != owned.root.pid], 'controller leaked a child before test cleanup'
            cleanup = owned.close()
            assert owned.process.returncode == 0, stderr.read_text()
    report.write_text(json.dumps({'passed': True, 'cleanup': cleanup}) + '\n')


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser()
    parser.add_argument('--node', type=Path)
    parser.add_argument('--controller', action='store_true')
    parser.add_argument('--stdout', type=Path)
    parser.add_argument('--stderr', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.node is not None: run_peer(args.node)
    elif args.controller:
        run_controller(args.command[1:] if args.command[:1] == ['--'] else args.command, args.stdout, args.stderr, args.report)

#!/usr/bin/env python3
"""Managed-soak orchestrator (WP-09 §13.6).

Runs the capsid managed host against the soak fixture for a fixed wall-clock
duration and continuously exercises the seven §13.6 dimensions:

  cancel/timeout    aborted mid-flight data-plane requests
  sse               slow-read streaming endpoint stays alive
  slow client       trickle-read of a 4 MiB body completes intact
  replacement       fresh deploy, SIGKILL every worker, the pool recovers
                    and keeps serving
  queue fairness    32 concurrent /slow requests all complete
  secret rotation   rotate the secret file, publish a fresh version, the
                    served marker follows the new value
  memory/token      soak-memory-waves binary: cancel waves + memory metrics
                    convergence (heap objects/properties/used plateau)

Versioning model (from the host semantics): a Version ID is immutable once
published, and the generation identity includes the secret revision — so a
version can only be served while its secret file is unchanged. Rotating the
secret therefore REQUIRES a fresh version ID; every deploy in the soak
publishes a new version (v2, v3, ...) and old versions are never touched
again. This is the property the 24h/72h schedule exercises end to end.

Usage:
  run_managed_soak.py \
      --host <capsid-host> --worker <capsid-worker> \
      --fixture <soak-app.js> --memory-waves <soak-memory-waves> \
      --work-dir <dir> --listen-port <port> --minutes <N>

Invariants are checked every cycle; the first violation prints a FAIL line
and exits 1. Evidence (per-cycle counters, RSS series, memory-wave JSON) is
written to <work-dir>/soak-evidence.json on success. --minutes 1 is a smoke
run; 1440/4320 are the 24h/72h schedules.

Python 3 stdlib only. The admin API is a Unix socket; the data plane is TCP.
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

APP = "soak"
# v1 is pre-created for the initial deploy; every soak deploy publishes a
# fresh version from the counter on.
INITIAL_VERSION = "v1"

# Path-mode routing serves the app under the /@capsid/{app} prefix.
DATA_PREFIX = "/@capsid/" + APP

ADMIN_TIMEOUT_S = 30


def fail(message):
    print("FAIL: " + message, flush=True)
    sys.exit(1)


class AdminClient:
    """Minimal HTTP/1.1 client for the Unix-socket admin API."""

    def __init__(self, sock_path):
        self.sock_path = sock_path

    def request(self, method, target, body=None):
        headers = "Host: local\r\n"
        if body is not None:
            headers += ("Content-Type: application/json\r\n"
                        "Content-Length: {}\r\n".format(len(body)))
        raw = ("{} {} HTTP/1.1\r\n{}Connection: close\r\n\r\n").format(
            method, target, headers)
        if body is not None:
            raw += body
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(ADMIN_TIMEOUT_S)
        sock.connect(self.sock_path)
        sock.sendall(raw.encode("utf-8"))
        chunks = []
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
        sock.close()
        response = b"".join(chunks).decode("utf-8", "replace")
        head, _, rest = response.partition("\r\n\r\n")
        return int(head.split(" ", 2)[1]), rest

    def deploy(self, version):
        status, body = self.request(
            "POST", "/v1/deploy",
            json.dumps({"app": APP, "version": version}))
        if status != 202:
            fail("deploy {}: HTTP {}: {}".format(version, status, body))
        return json.loads(body)["operationId"]

    def wait_terminal(self, operation):
        deadline = time.monotonic() + ADMIN_TIMEOUT_S
        while True:
            status, body = self.request(
                "GET", "/v1/operations/" + operation)
            if status != 200:
                fail("operation {}: HTTP {}".format(operation, status))
            state = json.loads(body).get("state")
            if state == "active":
                return True
            if state == "failed":
                return False
            if time.monotonic() > deadline:
                fail("operation {} did not become terminal".format(operation))
            time.sleep(0.1)

    def app_status(self):
        status, body = self.request("GET", "/v1/apps/" + APP)
        if status != 200:
            fail("app status: HTTP {}".format(status))
        return json.loads(body)


def http_get(port, path, timeout=10.0):
    """Returns (status, body bytes)."""
    try:
        with urllib.request.urlopen(
                "http://127.0.0.1:{}{}".format(port, path),
                timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def http_post(port, path, body, timeout=10.0):
    request = urllib.request.Request(
        "http://127.0.0.1:{}{}".format(port, path),
        data=body.encode("utf-8"))
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def worker_pids(host_pid):
    """Direct child PIDs of the host (the worker pool)."""
    try:
        output = subprocess.run(
            ["ps", "-o", "pid=,ppid=", "--ppid", str(host_pid)],
            capture_output=True, text=True, timeout=5).stdout
    except subprocess.SubprocessError:
        return []
    pids = []
    for line in output.splitlines():
        parts = line.split()
        if len(parts) == 2:
            pids.append(int(parts[0]))
    return pids


def rss_bytes(pid):
    try:
        with open("/proc/{}/status".format(pid), "r") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        pass
    return None


class Soak:
    def __init__(self, args, admin, port, host_pid):
        self.args = args
        self.admin = admin
        self.port = port
        self.host_pid = host_pid
        self.next_version = 2
        self.counts = {}
        self.rss_series = []
        self.memory_waves = []

    def count(self, key, amount=1):
        self.counts[key] = self.counts.get(key, 0) + amount

    def publish_fresh_version(self):
        """Creates a new applications/soak/vN tree and deploys it."""
        version = "v{}".format(self.next_version)
        self.next_version += 1
        version_dir = os.path.join(
            self.args.applications_dir, APP, version)
        os.makedirs(version_dir, mode=0o700, exist_ok=True)
        with open(os.path.join(version_dir, "capsid.json"), "w") as handle:
            json.dump(self.args.capsid_json, handle)
        shutil.copyfile(self.args.fixture,
                        os.path.join(version_dir, "bundle.mjs"))
        operation = self.admin.deploy(version)
        if not self.admin.wait_terminal(operation):
            fail("deploy {} failed".format(version))
        return version

    # ---- dimension drivers ----

    def dim_cancel_timeout(self):
        # Abort /slow requests mid-flight; the host and worker must
        # survive and keep serving.
        for _ in range(4):
            try:
                sock = socket.create_connection(
                    ("127.0.0.1", self.port), timeout=5)
                sock.sendall(
                    ("GET {}/slow?ms=3000 HTTP/1.1\r\nHost: local\r\n\r\n"
                     .format(DATA_PREFIX)).encode())
                time.sleep(0.05)
                sock.close()  # abort mid-flight
            except OSError:
                fail("cancel: connection setup failed")
            self.count("cancel_aborted")
        status, body = http_post(self.port, DATA_PREFIX + "/echo", "probe")
        if status != 200 or body != b"echo:probe":
            fail("cancel: server did not survive aborts "
                 "(POST /echo -> {} {!r})".format(status, body[:32]))
        self.count("cancel_survive_check")

    def dim_sse(self):
        # Slow-read an SSE stream: every tick must arrive and the stream
        # must end cleanly.
        try:
            sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            sock.sendall(("GET {}/sse HTTP/1.1\r\nHost: local\r\n\r\n"
                          .format(DATA_PREFIX)).encode())
            data = b""
            sock.settimeout(15)
            deadline = time.monotonic() + 15
            while b"data: tick-2" not in data:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
                time.sleep(0.15)  # slow read
                if time.monotonic() > deadline:
                    break
            sock.close()
        except OSError as error:
            fail("sse: transport error: {}".format(error))
        if data.count(b"data: tick-") < 3:
            fail("sse: expected 3 ticks, got {!r}".format(data[-200:]))
        self.count("sse_ticks", 3)

    def dim_slow_client(self):
        # Trickle-read the 4 MiB body; the stream must complete intact.
        # The listener serializes chunked (the worker emits no
        # Content-Length for a buffered body), so the wire body is
        # de-framed here like a real HTTP client would. The read loop
        # ends at the chunked terminal chunk — like dim_sse's tick
        # marker — because the connection stays keep-alive and an idle
        # recv would time out waiting for bytes that never come.
        wire = b""
        terminal_seen = False
        deadline = time.monotonic() + 30
        try:
            sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
            sock.sendall(("GET {}/big HTTP/1.1\r\nHost: local\r\n"
                          "Connection: close\r\n\r\n"
                          .format(DATA_PREFIX)).encode())
            sock.settimeout(10)
            while time.monotonic() < deadline:
                chunk = sock.recv(65536)
                if not chunk:
                    break  # the host honored Connection: close
                wire += chunk
                if wire.endswith(b"\r\n0\r\n\r\n"):
                    terminal_seen = True
                    break  # the chunked terminal chunk has arrived
                time.sleep(0.05)
            sock.close()
        except OSError as error:
            fail("slow-client: transport error: {}".format(error))
        if not terminal_seen:
            fail("slow-client: response did not reach its terminal chunk "
                 "({} wire bytes)".format(len(wire)))
        head_end = wire.find(b"\r\n\r\n")
        if head_end == -1:
            fail("slow-client: no response head received")
        head = wire[:head_end].lower()
        if b"\r\ntransfer-encoding: chunked" not in head:
            fail("slow-client: expected a chunked response, got {!r}".format(
                head[:120]))
        payload = b""
        pos = head_end + 4
        wire_len = len(wire)
        for _ in range(1 << 16):  # framing safety bound
            line_end = wire.find(b"\r\n", pos)
            if line_end == -1:
                fail("slow-client: truncated chunk header")
            size_field = wire[pos:line_end].split(b";")[0].strip()
            try:
                size = int(size_field, 16)
            except ValueError:
                fail("slow-client: bad chunk size line {!r}".format(
                    size_field[:64]))
            if size == 0:
                break
            if line_end + 2 + size > wire_len:
                fail("slow-client: chunk body truncated (at {} of {})".format(
                    len(payload), size))
            payload += wire[line_end + 2: line_end + 2 + size]
            pos = line_end + 2 + size + 2
        expected = 4 * 1024 * 1024
        if len(payload) != expected or payload != b"\x53" * expected:
            fail("slow-client: body {}/{} bytes or pattern wrong".format(
                len(payload), expected))
        self.count("slow_client_bytes", len(payload))

    def dim_replacement(self, cycle):
        # Publish a fresh version, then SIGKILL every worker child; the
        # pool must replace them and keep serving.
        version = self.publish_fresh_version()
        status = self.admin.app_status()
        if not status.get("active") or status.get("version") != version:
            fail("replacement: status did not publish {}: {!r}".format(
                version, status))
        pids = worker_pids(self.host_pid)
        if not pids:
            fail("replacement: no worker children visible")
        for pid in pids:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 30
        ok = False
        while time.monotonic() < deadline:
            try:
                status, body = http_post(self.port, DATA_PREFIX + "/echo",
                                         "postkill")
                if status == 200 and body == b"echo:postkill":
                    ok = True
                    break
            except (urllib.error.URLError, socket.timeout):
                pass
            time.sleep(0.2)
        if not ok:
            fail("replacement: data plane did not recover after SIGKILL")
        self.count("replacement_cycles")

    def dim_queue_fairness(self):
        urls = ["http://127.0.0.1:{}{}/slow?ms=150".format(
            self.port, DATA_PREFIX)] * 32

        def one(url):
            try:
                with urllib.request.urlopen(url, timeout=20) as response:
                    return response.status, response.read()
            except urllib.error.HTTPError as error:
                return error.code, error.read()
            except (urllib.error.URLError, socket.timeout):
                return 0, b""

        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            results = list(pool.map(one, urls))
        ok = all(status == 200 and body == b"slow-ok"
                 for status, body in results)
        if not ok:
            fail("queue fairness: {} of 32 concurrent /slow failed".format(
                sum(1 for status, _ in results if status != 200)))
        self.count("fairness_requests", 32)

    def dim_secret_rotation(self, cycle):
        # Rotate the secret file, then publish a FRESH version (the
        # committed generation of any older version is pinned to the
        # earlier secret revision and cannot be re-published).
        value = "rotated-{}".format(cycle)
        secret_path = os.path.join(
            self.args.secrets_dir, APP, "APP_SOAK_MARKER")
        with open(secret_path, "w") as handle:
            handle.write(value)
        version = self.publish_fresh_version()
        status = self.admin.app_status()
        if not status.get("active") or status.get("version") != version:
            fail("secret rotation: status did not publish {}: {!r}".format(
                version, status))
        http_status, body = http_get(self.port, DATA_PREFIX + "/marker",
                                     timeout=10)
        if http_status != 200 or body.decode("utf-8", "replace") != value:
            fail("secret rotation: served marker {!r} != {!r}".format(
                body[:32], value))
        self.count("secret_rotations")

    def dim_memory_waves(self):
        result = subprocess.run(
            [self.args.memory_waves, self.args.worker,
             self.args.fixture, "6", "200"],
            capture_output=True, text=True, timeout=120)
        try:
            report = json.loads(result.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            fail("memory waves: unparseable output: {}".format(
                result.stdout[-300:]))
        self.memory_waves.append(report)
        if not report.get("converged"):
            fail("memory waves: heap did not converge: {}".format(
                report.get("reason")))
        self.count("memory_waves")

    # ---- evidence ----

    def snapshot_rss(self):
        self.rss_series.append({
            "host": rss_bytes(self.host_pid),
            "workers": [rss_bytes(pid) for pid in worker_pids(self.host_pid)],
        })

    def evidence(self):
        return {
            "application": APP,
            "cycles": self.counts.get("cycles", 0),
            "counts": self.counts,
            "rss_series": self.rss_series,
            "memory_waves": self.memory_waves,
        }


CAPSID_JSON = {
    "apiVersion": "capsid/app-v1",
    "entry": "bundle.mjs",
    "permissions": {
        "modules": ["capsid:env"],
        "env": {
            "APP_SOAK_MARKER": {"valueFrom": "APP_SOAK_MARKER"},
        },
    },
    "pool": {"minReady": 1, "maxWorkers": 1},
}


def build_fixture_tree(work_dir, fixture_path):
    """applications/soak/{v1}, secrets/soak/APP_SOAK_MARKER, state/run."""
    applications = os.path.join(work_dir, "applications")
    secrets = os.path.join(work_dir, "secrets")
    state = os.path.join(work_dir, "state")
    run = os.path.join(work_dir, "run")
    for directory in (applications, secrets, state, run):
        os.makedirs(directory, mode=0o700, exist_ok=True)
    version_dir = os.path.join(applications, APP, INITIAL_VERSION)
    os.makedirs(version_dir, mode=0o700, exist_ok=True)
    with open(os.path.join(version_dir, "capsid.json"), "w") as handle:
        json.dump(CAPSID_JSON, handle)
    shutil.copyfile(fixture_path, os.path.join(version_dir, "bundle.mjs"))
    secret_app_dir = os.path.join(secrets, APP)
    os.makedirs(secret_app_dir, mode=0o700, exist_ok=True)
    with open(os.path.join(secret_app_dir, "APP_SOAK_MARKER"), "w") as handle:
        handle.write("v1-secret")
    return applications, secrets, state, run


def write_host_json(work_dir, port, admin_sock):
    document = {
        "apiVersion": "capsid/host-v1",
        "applicationsRoot": os.path.join(work_dir, "applications"),
        "stateRoot": os.path.join(work_dir, "state"),
        "secretRootTemplate": os.path.join(work_dir, "secrets",
                                           "{application}"),
        "admin": {"unix": admin_sock, "mode": "0600"},
        "permissions": {
            "modules": ["capsid:env"],
            "environmentNames": ["APP_*"],
            "fsReadRoots": [],
            "fetchTargets": [],
            "storageNamespaces": [],
            "stdioStreams": [],
        },
        "isolation": {"mode": "strict", "required": []},
        "maximums": {
            "request": {
                "timeout": "30s",
                "maxInflightPerWorker": 10000,
            },
        },
        "listeners": [{
            "name": "soak",
            "tcp": "127.0.0.1:{}".format(port),
            "publicScheme": "http",
            "publicAuthority": "127.0.0.1:{}".format(port),
            "trusted": False,
            "routing": {"mode": "path", "suffix": "/"},
            "limits": {},
        }],
        # §9.4: a zero-downtime replace (secret rotation, SIGKILL
        # replacement) warms the new pool while the old one still serves —
        # the ledger refuses the reserve without explicit surge headroom.
        "capacity": {"workersTotal": 1, "startupsConcurrent": 1,
                     "activationSurgeWorkers": 1},
    }
    path = os.path.join(work_dir, "host.json")
    with open(path, "w") as handle:
        json.dump(document, handle)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="capsid-host binary")
    parser.add_argument("--worker", required=True, help="capsid-worker binary")
    parser.add_argument("--fixture", required=True, help="soak-app.js path")
    parser.add_argument("--memory-waves", required=True,
                        help="soak-memory-waves binary")
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--minutes", type=int, default=1,
                        help="soak duration in minutes (1 = smoke, "
                             "1440 = 24h, 4320 = 72h)")
    args = parser.parse_args()

    for binary in (args.host, args.worker, args.fixture, args.memory_waves):
        if not os.path.isfile(binary):
            fail("missing input: {}".format(binary))
    os.makedirs(args.work_dir, mode=0o700, exist_ok=True)

    applications, secrets, state, run = build_fixture_tree(
        args.work_dir, args.fixture)
    args.applications_dir = applications
    args.secrets_dir = secrets
    args.capsid_json = CAPSID_JSON
    admin_sock = os.path.join(run, "admin-{}.sock".format(uuid.uuid4().hex[:8]))
    host_config = write_host_json(args.work_dir, args.listen_port, admin_sock)

    stderr_path = os.path.join(args.work_dir, "host.stderr")
    with open(stderr_path, "wb") as stderr:
        host = subprocess.Popen(
            [args.host, "--mode", "managed",
             "--host-config", host_config, "--worker", args.worker],
            stdout=subprocess.DEVNULL, stderr=stderr)

    try:
        deadline = time.monotonic() + ADMIN_TIMEOUT_S
        while not os.path.exists(admin_sock):
            if host.poll() is not None:
                fail("capsid-host exited during startup (see {})".format(
                    stderr_path))
            if time.monotonic() > deadline:
                fail("admin socket never appeared (see {})".format(stderr_path))
            time.sleep(0.1)

        admin = AdminClient(admin_sock)
        operation = admin.deploy(INITIAL_VERSION)
        if not admin.wait_terminal(operation):
            fail("initial deploy failed (see {})".format(stderr_path))
        soak = Soak(args, admin, args.listen_port, host.pid)

        end = time.monotonic() + args.minutes * 60
        cycle = 0
        while time.monotonic() < end:
            cycle += 1
            soak.dim_cancel_timeout()
            soak.dim_sse()
            soak.dim_slow_client()
            soak.dim_secret_rotation(cycle)
            soak.dim_replacement(cycle)
            soak.dim_queue_fairness()
            soak.dim_memory_waves()
            soak.snapshot_rss()
            soak.counts["cycles"] = cycle
            print("cycle {} ok: {}".format(
                cycle, json.dumps(soak.counts)), flush=True)

        evidence = soak.evidence()
        evidence_path = os.path.join(args.work_dir, "soak-evidence.json")
        with open(evidence_path, "w") as handle:
            json.dump(evidence, handle, indent=2)
        print("SOAK PASS: {} cycles in {}s; evidence: {}".format(
            cycle, args.minutes * 60, evidence_path), flush=True)
    finally:
        host.send_signal(signal.SIGKILL)
        host.wait(timeout=10)


if __name__ == "__main__":
    main()

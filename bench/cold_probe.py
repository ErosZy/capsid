"""Actual owned cold C-ABI/HTTP probes and private bytecode compilation."""
from dataclasses import asdict
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import statistics
import tempfile
import time

from cold_control import EXPECTED_BODY, checked_round, identity
from measurement_runtime import OwnedProcess, PortAllocator, ProbeError, ProcFS, stable_pss


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def wait_exit(owned, deadline):
    while owned.alive():
        owned.discover()
        if time.monotonic() >= deadline:
            raise ProbeError("cold-deadline", "cold child exceeded the probe deadline")
        time.sleep(.005)


def exact_request(port, *, timeout=5):
    started = time.monotonic()
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        connection.request("GET", "/@capsid/cold-start", headers={"Connection": "close"})
        response = connection.getresponse()
        body = response.read(len(EXPECTED_BODY) + 1)
        if response.status != 200 or body != EXPECTED_BODY:
            raise ProbeError("cold-response", "cold request returned wrong status or exact body", status=response.status,
                             bodySHA256=hashlib.sha256(body).hexdigest())
        return {"latency_ms": (time.monotonic() - started) * 1000, "status": 200, "ok": True,
                "bodySHA256": hashlib.sha256(body).hexdigest()}
    except (OSError, http.client.HTTPException) as exc:
        raise ProbeError("cold-request", f"cold HTTP request failed: {exc}") from exc
    finally:
        connection.close()


def wait_ready(owned, port, stdout, marker, deadline):
    while time.monotonic() < deadline:
        if not owned.alive():
            raise ProbeError("cold-startup", "HTTP stack exited before readiness")
        has_marker = not marker or b"READY" in stdout.read_bytes().splitlines()
        if has_marker and owned.listeners_owned([port]):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    return time.monotonic()
            except OSError:
                pass  # A readiness connect is not a request or a body retry.
        time.sleep(.005)
    raise ProbeError("cold-startup", "HTTP stack did not become ready before its deadline")


def compile_fixture(binary, source, archive, cpuset, *, timeout=120):
    """Compile all three outputs on Linux native storage, then archive bytes.

    WSL's mounted NTFS can reject RENAME_NOREPLACE. Do not disable the compiler's
    atomic/no-overwrite contract or reuse a shared APPS bytecode artifact.
    """
    archive.mkdir(parents=True)
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    outputs = ("bundle.qjsb", "attestation.json", "message.bin")
    with tempfile.TemporaryDirectory(prefix="capsid-cold-compile-", dir="/tmp") as temporary:
        folder = Path(temporary)
        command = ["taskset", "-c", cpuset, str(binary), "--source", str(source), "--source-name", source.as_uri(),
                   "--application", "coldbench", "--version", "1", "--key-id", "bench",
                   "--bytecode-out", str(folder / outputs[0]), "--attestation-out", str(folder / outputs[1]),
                   "--signing-message-out", str(folder / outputs[2])]
        write_json(archive / "launch.json", {"command": command, "sourceSHA256": before,
                   "sourceName": source.as_uri(), "outputStorage": "owned-native-linux-temporary-directory"})
        with (archive / "stdout.log").open("xb") as stdout, (archive / "stderr.log").open("xb") as stderr:
            owned = None
            try:
                owned = OwnedProcess(command, stdout=stdout, stderr=stderr)
                wait_exit(owned, time.monotonic() + timeout)
            finally:
                if owned is not None:
                    write_json(archive / "cleanup.json", owned.close())
        if owned.process.returncode != 0:
            raise ProbeError("cold-compile", "bytecode compiler failed", exitCode=owned.process.returncode)
        for name in outputs:
            if not (folder / name).is_file() or (folder / name).is_symlink():
                raise ProbeError("cold-compile", "compiler omitted an output")
            shutil.copyfile(folder / name, archive / name)
    if hashlib.sha256(source.read_bytes()).hexdigest() != before:
        raise ProbeError("fixture-changed", "source changed during bytecode compilation")
    attestation = json.loads((archive / "attestation.json").read_text())
    bytecode = archive / "bundle.qjsb"
    if (attestation.get("sourceName") != source.as_uri() or attestation.get("sourceSha256") != "sha256:" + before or
            attestation.get("bytecodeSha256") != "sha256:" + hashlib.sha256(bytecode.read_bytes()).hexdigest()):
        raise ProbeError("cold-compile", "compiler output identities disagree with its measured source/bytecode")
    return bytecode


class ColdProbe:
    def __init__(self, *, binaries, apps, compiled, out, cpuset, warm_requests=50, round_timeout=180,
                 startup_timeout=30, settle_seconds=1, pss_interval=1, pss_attempts=8):
        self.binaries, self.apps, self.compiled = binaries, Path(apps), compiled
        self.out, self.cpuset, self.warm_requests = Path(out), cpuset, warm_requests
        self.round_timeout, self.startup_timeout = round_timeout, startup_timeout
        self.settle_seconds, self.pss_interval, self.pss_attempts = settle_seconds, pss_interval, pss_attempts
        self.ports = PortAllocator()
        self.index = 0

    def prepare_http(self, stack, size, directory, port):
        runtime = directory / "runtime"
        runtime.mkdir()
        source = (self.apps / f"app-{stack}-{size}.mjs").read_text(encoding="utf-8")
        if stack == "node":
            old = 'server.listen(18990, "127.0.0.1", () => console.log("READY"));'
            new = old.replace("18990", str(port))
            argv = [str(self.binaries["node"]), str(runtime / "bundle.mjs")]
        elif stack == "deno":
            old = 'Deno.serve({ port: 18991, hostname: "127.0.0.1" },'
            new = old.replace("18991", str(port))
            argv = [str(self.binaries["deno"]), "run", "--no-check", "--allow-net=127.0.0.1:" + str(port), str(runtime / "bundle.mjs")]
        else:
            old = None
            config = ('using Workerd = import "/workerd/workerd.capnp";\n'
                'const config :Workerd.Config = (services = [(name = "main", worker = .worker)], '
                'sockets = [(name = "http", address = "127.0.0.1:' + str(port) + '", http = (), service = "main")]);\n'
                'const worker :Workerd.Worker = (compatibilityDate = "2026-08-20", '
                'modules = [(name = "main", esModule = embed "bundle.mjs")]);\n')
            (runtime / "server.capnp").write_text(config, encoding="utf-8")
            argv = [str(self.binaries["workerd"]), "serve", str(runtime / "server.capnp")]
        if old is not None:
            if source.count(old) != 1:
                raise ProbeError("fixture-format", "cold fixture lacks its frozen listener entry")
            source = source.replace(old, new)
        (runtime / "bundle.mjs").write_text(source, encoding="utf-8")
        return ["taskset", "-c", self.cpuset, *argv]

    def __call__(self, *, stack, size, round_no):
        self.index += 1
        name = f"probe-{self.index:05d}-{stack}-{size}-r{round_no}"
        directory = self.out / name
        directory.mkdir()
        owned, lease, cleanup, result, failure = None, None, None, None, None
        deadline = time.monotonic() + self.round_timeout
        stdout_path, stderr_path = directory / "stdout.log", directory / "stderr.log"
        try:
            if stack.startswith("capsid-"):
                source = self.apps / f"app-capsid-{size}.mjs"
                mode = stack.removeprefix("capsid-")
                app = self.compiled[size] if mode == "bytecode" else source
                command = ["taskset", "-c", self.cpuset, str(self.binaries["cold-start"]), "--worker",
                    str(self.binaries["capsid-worker"]), "--mode", mode, "--input", str(app), "--source-name",
                    source.as_uri(), "--strict", "--iterations", "1", "--warm", str(self.warm_requests)]
            else:
                lease = self.ports.allocate(1)
                command = self.prepare_http(stack, size, directory, lease.ports[0])
            write_json(directory / "launch.json", {"command": command, "ports": lease.ports if lease else [],
                       "round": round_no, "stack": stack, "size": size})
            with stdout_path.open("xb") as stdout, stderr_path.open("xb") as stderr:
                if lease is not None: lease.release()
                owned = OwnedProcess(command, stdout=stdout, stderr=stderr)
                if stack.startswith("capsid-"):
                    wait_exit(owned, deadline)
                    rows = stdout_path.read_text(encoding="utf-8").splitlines()
                    if len(rows) != 1:
                        raise ProbeError("cold-tool", "C-ABI tool did not emit exactly one iteration")
                    result = json.loads(rows[0])
                    if (result.get("round") != 1 or type(result.get("round")) is not int or
                            result.get("mode") != mode or result.get("posture") != "c-abi-strict"):
                        raise ProbeError("cold-tool", "unexpected C-ABI tool iteration or posture")
                    members = result.get("measurementProcesses", [])
                    roots = [p for p in members if p.get("pid") == result.get("worker_pid")]
                    if len(roots) != 1:
                        raise ProbeError("cold-identity", "tool did not identify its actual worker")
                    result.update(measurementRoot=roots[0], probeProcess=asdict(owned.root), sourceProbeRound=1, transport="c-abi")
                else:
                    ready = wait_ready(owned, lease.ports[0], stdout_path, stack != "workerd",
                                       min(deadline, owned.started_at + self.startup_timeout))
                    first = exact_request(lease.ports[0], timeout=min(5, max(.01, deadline - time.monotonic())))
                    first_end = time.monotonic()
                    pinned = owned.pin_members()
                    latencies = []
                    with (directory / "requests.jsonl").open("x", encoding="utf-8") as requests:
                        requests.write(json.dumps(dict(first, request=1)) + "\n")
                        for index in range(self.warm_requests):
                            if time.monotonic() >= deadline: raise ProbeError("cold-deadline", "warm deadline exceeded")
                            response = exact_request(lease.ports[0], timeout=min(5, max(.01, deadline - time.monotonic())))
                            requests.write(json.dumps(dict(response, request=index + 2)) + "\n")
                            requests.flush()
                            latencies.append(response["latency_ms"])
                    time.sleep(self.settle_seconds)
                    def sample():
                        if time.monotonic() >= deadline: raise ProbeError("cold-deadline", "PSS deadline exceeded")
                        value = owned.sample()
                        with (directory / "pss-attempts.jsonl").open("a", encoding="utf-8") as samples:
                            samples.write(json.dumps(value) + "\n")
                        return value
                    pss = stable_pss(sample, interval=self.pss_interval, attempts=self.pss_attempts)
                    result = {"ok": True, "transport": "http", "total_ms": (first_end - owned.started_at) * 1000,
                              "ready_ms": (ready - owned.started_at) * 1000, "first_request_ms": first["latency_ms"],
                              "warm_ms": statistics.median(latencies), "warm_n": self.warm_requests,
                              "warm_latencies_ms": latencies, "measurementRoot": asdict(owned.root),
                              "probeProcess": asdict(ProcFS().identity(os.getpid())),
                              "measurementProcesses": [asdict(p) for p in pinned.values()],
                              "pss_kb": pss["pss_readings_kb"][0], "pss_readings_kb": pss["pss_readings_kb"],
                              "pssSamples": pss["samples"][-2:], "pssAttempts": len(pss["samples"]),
                              "correctness": {"ok": True, "errors_as_success": False, "responses_checked": self.warm_requests + 1,
                                              "errors": 0, "mismatches": 0, "timeouts": 0}}
        except BaseException as exc:
            failure = exc
        finally:
            if owned is not None:
                try:
                    cleanup = owned.close()
                    write_json(directory / "cleanup.json", cleanup)
                except BaseException as exc:
                    failure = exc
            if lease is not None: lease.release()
        if failure is None and stack.startswith("capsid-") and owned.process.returncode != 0:
            failure = ProbeError("cold-tool", "C-ABI tool failed", exitCode=owned.process.returncode)
        if failure is not None:
            record = failure.record() if isinstance(failure, ProbeError) else {
                "status": "error", "error": {"type": type(failure).__name__, "message": str(failure)}}
            write_json(directory / "result.json", dict(record, cleanup=cleanup))
            raise failure
        result.update(stack=stack, size=size, round=round_no, probeDirectory=name, cleanup=cleanup)
        observed = {(p.pid, p.start) for p in owned.known.values()}
        try:
            checked_round(result, self.warm_requests, observed=observed)
        except ProbeError as exc:
            write_json(directory / "result.json", dict(exc.record(), cleanup=cleanup))
            raise
        write_json(directory / "result.json", result)
        return result

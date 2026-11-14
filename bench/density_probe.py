"""Real managed/strict and workerd density probes; no production response changes."""
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict
from pathlib import Path
import errno
import hashlib
import http.client
import json
import os
import re
import shutil
import socket
import tempfile
import threading
import time
import uuid

from density_control import CounterLog, Coverage, MARKER, WorkerdReadyLog, wait_network_headroom
from measurement_runtime import OwnedProcess, ProbeError, stable_pss

# gen-cold-start-apps.py's frozen seed=7 response. Check the generated app
# itself in fixture tests; do not learn an expected response from the SUT.
EXPECTED_BODY = b'{"status":"ok","total":2039,"count":16}'


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def instrument_fixture(source, nonce):
    marker = "export default {"
    if source.count(marker) != 1 or "__density" in source:
        raise ProbeError("fixture-format", "density instrumentation requires the frozen generated fixture")
    result = source.replace(marker, "const __densityApp = {", 1)
    return result + "\n" + "\n".join([
        "let __densityId, __densityCount = 0;",
        "export default { async fetch(req, env, ctx) {",
        "  const response = await __densityApp.fetch(req, env, ctx);",
        "  if (req.headers.get('x-capsid-bench-nonce') === " + json.dumps(nonce) + ") {",
        "    if (!__densityId) __densityId = crypto.randomUUID();",
        "    ++__densityCount;",
        "    console.error(" + json.dumps(MARKER) + " + JSON.stringify({nonce:" + json.dumps(nonce) +
        ", worker:__densityId, count:__densityCount}));",
        "  }",
        "  return response;",
        "} };", "",
    ])


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path, timeout=2):
        super().__init__("localhost", timeout=timeout)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def admin_listener_ready(path, deadline):
    """A bound pathname is not a listening service; never retry a business RPC."""
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        return False
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            probe.settimeout(min(2, remaining))
            probe.connect(str(path))
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ECONNREFUSED):
            return False
        raise
    return True


def admin_request(path, method, route, body=None, accepted=(200,)):
    connection = UnixHTTP(path)
    try:
        connection.request(method, route, body=body, headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        payload = response.read(1024 * 1024)
        if response.status not in accepted:
            raise ProbeError("activation", f"managed Admin API returned {response.status}")
        return payload
    finally:
        connection.close()


def validated_request(port, nonce, *, start=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        if start is not None:
            connection.connect()
            start.wait(timeout=5)
        connection.request("GET", "/@capsid/orders/bench/json",
                           headers={"Connection": "close", "x-capsid-bench-nonce": nonce})
        response = connection.getresponse()
        body = response.read(len(EXPECTED_BODY) + 1)
        if response.status != 200 or body != EXPECTED_BODY:
            raise ProbeError("correctness", "density request returned the wrong status or exact body",
                             status=response.status, actualSHA256=hashlib.sha256(body).hexdigest())
        return {"port": port, "status": response.status, "ok": True,
                "bodySHA256": hashlib.sha256(body).hexdigest()}
    except (OSError, http.client.HTTPException, threading.BrokenBarrierError) as exc:
        if start is not None:
            start.abort()
        raise ProbeError("request-failed", f"density request failed: {exc}") from exc
    finally:
        connection.close()


def validated_wave(ports, nonce, *, connect_parallel=64):
    """Connect a bounded wave, emit every request, then validate responses.

    Large waves use a single sender so Python thread wakeups cannot turn the
    intended burst into hundreds of sequential fast request/response pairs.
    The response parser and exact-byte requirements match validated_request.
    """
    sockets = []
    try:
        failure = None
        with ThreadPoolExecutor(max_workers=min(connect_parallel, len(ports))) as executor:
            pending = [executor.submit(socket.create_connection, ("127.0.0.1", port), timeout=5)
                       for port in ports]
            for future in pending:
                try:
                    sockets.append(future.result())
                except OSError as exc:
                    failure = failure or exc
        if failure is not None:
            raise failure
        packets = [("GET /@capsid/orders/bench/json HTTP/1.1\r\n"
                    f"Host: 127.0.0.1:{port}\r\nConnection: close\r\n"
                    f"x-capsid-bench-nonce: {nonce}\r\n\r\n").encode("ascii") for port in ports]
        for client, packet in zip(sockets, packets):
            client.sendall(packet)
        observations = []
        for port, client in zip(ports, sockets):
            with http.client.HTTPResponse(client) as response:
                response.begin()
                body = response.read(len(EXPECTED_BODY) + 1)
                if response.status != 200 or body != EXPECTED_BODY:
                    raise ProbeError("correctness", "density request returned the wrong status or exact body",
                                     status=response.status, actualSHA256=hashlib.sha256(body).hexdigest())
                observations.append({"port": port, "status": response.status, "ok": True,
                                     "bodySHA256": hashlib.sha256(body).hexdigest()})
        return observations
    except (OSError, http.client.HTTPException) as exc:
        raise ProbeError("request-failed", f"density wave failed: {exc}") from exc
    finally:
        for client in sockets:
            client.close()


class DensityProbe:
    def __init__(self, *, host, worker, workerd, apps, out, cpuset, warm_requests=100,
                 parallel=64, startup_timeout=120, coverage_timeout=300,
                 settle_seconds=1, pss_interval=1, pss_attempts=8):
        self.host, self.worker, self.workerd = map(lambda p: Path(p).resolve(), (host, worker, workerd))
        self.apps, self.out, self.cpuset = Path(apps), Path(out), cpuset
        self.warm_requests, self.parallel = warm_requests, parallel
        self.startup_timeout, self.coverage_timeout = startup_timeout, coverage_timeout
        self.settle_seconds, self.pss_interval, self.pss_attempts = settle_seconds, pss_interval, pss_attempts

    def prepare(self, folder, stack, size, count, ports, nonce):
        original = self.apps / f"app-{stack}-{size}.mjs"
        source = instrument_fixture(original.read_text(encoding="utf-8"), nonce)
        if stack == "capsid":
            app = folder / "apps/orders/v1"
            for directory in (folder / "apps", app.parent, app, folder / "state",
                              folder / "secrets", folder / "secrets/orders", folder / "run"):
                directory.mkdir(parents=True, exist_ok=True, mode=0o700)
                directory.chmod(0o700)
            (app / "bundle.mjs").write_text(source, encoding="utf-8")
            write_json(app / "capsid.json", {"apiVersion": "capsid/app-v1", "entry": "bundle.mjs",
                                             "pool": {"minReady": count, "maxWorkers": count}})
            write_json(folder / "host.json", {"apiVersion": "capsid/host-v1",
                "applicationsRoot": str(folder / "apps"), "stateRoot": str(folder / "state"),
                "secretRootTemplate": str(folder / "secrets/{application}"),
                "admin": {"unix": str(folder / "run/admin.sock")},
                "capacity": {"workersTotal": count, "startupsConcurrent": min(8, count)},
                "listeners": [{"name": "density", "tcp": f"127.0.0.1:{ports[0]}", "publicScheme": "http",
                               "publicAuthority": "bench.invalid", "trusted": False,
                               "routing": {"mode": "path", "suffix": ""}}]})
            for file in (app / "bundle.mjs", app / "capsid.json", folder / "host.json"):
                file.chmod(0o600)
            return [str(self.host), "--mode", "managed", "--host-config", str(folder / "host.json"),
                    "--worker", str(self.worker)]
        (folder / "bundle.mjs").write_text(source, encoding="utf-8")
        services = ", ".join(f'(name = "w{i}", worker = .w{i})' for i in range(count))
        sockets = ", ".join(f'(name = "s{i}", address = "127.0.0.1:{port}", http = (), service = "w{i}")'
                            for i, port in enumerate(ports))
        config = ['using Workerd = import "/workerd/workerd.capnp";',
                  f'const config :Workerd.Config = (services = [{services}], sockets = [{sockets}]);']
        config += [f'const w{i} :Workerd.Worker = (compatibilityDate = "2026-08-20", '
                   'modules = [(name = "main", esModule = embed "bundle.mjs")]);' for i in range(count)]
        (folder / "workerd.capnp").write_text("\n".join(config) + "\n", encoding="utf-8")
        return [str(self.workerd), "serve", str(folder / "workerd.capnp"), "--control-fd=1"]

    def wait_ready(self, tree, folder, stack, ports, *, control_log=None, deadline=None):
        if deadline is None:
            deadline = time.monotonic() + self.startup_timeout
        deployed = stack != "capsid"
        if stack == "workerd" and control_log is None:
            raise ProbeError("readiness-protocol", "workerd requires its owned startup control log")
        control = WorkerdReadyLog(control_log, ports) if stack == "workerd" else None
        while time.monotonic() < deadline:
            if not tree.alive():
                raise ProbeError("process-exited", "density serving process exited before readiness")
            if stack == "capsid":
                admin = folder / "run/admin.sock"
                if deployed or admin_listener_ready(admin, deadline):
                    if not deployed:
                        admin_request(admin, "POST", "/v1/deploy", b'{"app":"orders","version":"v1"}', accepted=(202,))
                        deployed = True
                    state = admin_request(admin, "GET", "/v1/apps/orders")
                    if not re.search(rb'"active"\s*:\s*true', state):
                        time.sleep(.05)
                        continue
                else:
                    time.sleep(.05)
                    continue
            # workerd binds sockets before initializing services. Its control-FD
            # listen events occur only after that initialization, without an HTTP
            # request or prewarming. Keep the same absolute startup deadline.
            if deployed and (control is None or control.poll()) and tree.listeners_owned(ports):
                return ({"method": "workerd-control-listen-v1", "sockets": len(ports),
                         "beforeTraffic": True, "deadlineSeconds": self.startup_timeout}
                        if control is not None else None)
            time.sleep(.05)
        raise ProbeError("readiness-timeout", "density pool did not become ready within its deadline")

    def warmup_concurrency(self, count):
        # Density conditioning must reach every worker before PSS is read.
        # Up to 128 clients per worker leave room for fast completions;
        # this is separate from the QPS lane's fixed 64 in-flight requests.
        return max(self.parallel, count * 128)

    def initial_wave_requests(self, stack, count, maximum, concurrency):
        return min(concurrency, maximum if count > self.parallel else max(1, count))

    def cover(self, tree, counter, stack, ports, count, target, nonce, output):
        deadline = time.monotonic() + self.coverage_timeout
        responses, maximum = 0, max(64, count * target * (32 if target > 1 else 8))
        concurrency = self.warmup_concurrency(count)
        batch_limit = self.initial_wave_requests(stack, count, maximum, concurrency)
        maximum_wave = 0
        with ThreadPoolExecutor(max_workers=self.parallel) as executor:
            while not counter.coverage.complete(target):
                previous_minimum = (min(counter.coverage.counts.values())
                                    if len(counter.coverage.counts) == count else 0)
                if not tree.alive():
                    raise ProbeError("process-exited", "density pool exited during request coverage")
                if responses >= maximum or time.monotonic() >= deadline:
                    raise ProbeError("coverage-deadline", "per-worker request target was not reached",
                                     responses=responses, observed=counter.coverage.counts)
                batch = min(batch_limit, maximum - responses)
                if stack == "workerd":
                    batch = min(batch, count * target - responses)
                    if batch <= 0:
                        raise ProbeError("coverage-incomplete", "workerd cells did not report their validated requests")
                selected = [ports[(responses + i) % len(ports)] for i in range(batch)]
                maximum_wave = max(maximum_wave, batch)
                if batch > self.parallel:
                    observations = validated_wave(selected, nonce, connect_parallel=self.parallel)
                else:
                    start = threading.Barrier(batch)
                    futures = [executor.submit(validated_request, port, nonce, start=start) for port in selected]
                    observations = (future.result() for future in futures)
                for response in observations:
                    responses += 1
                    output.write(json.dumps(dict(response, sequence=responses, waveRequests=batch,
                                                 warmupConcurrencyLimit=concurrency), sort_keys=True) + "\n")
                output.flush()
                # LOG delivery may follow the response, but a dropped log must
                # fail coverage instead of silently substituting total traffic.
                log_deadline = min(deadline, time.monotonic() + 3)
                while True:
                    counter.poll()
                    if sum(counter.coverage.counts.values()) == responses:
                        break
                    if time.monotonic() >= log_deadline:
                        raise ProbeError("coverage-count", "missing per-worker counter records after validated responses")
                    time.sleep(.01)
                minimum = (min(counter.coverage.counts.values())
                           if len(counter.coverage.counts) == count else 0)
                if stack == "capsid" and minimum <= previous_minimum and not counter.coverage.complete(target):
                    # Equal-load ties favor the lowest slot. A fast response
                    # can finish before later clients even connect; synchronize
                    # the connected clients and widen a starved wave. The
                    # density-only limit scales with the pool; the original
                    # request budget and per-worker assertions are retained.
                    batch_limit = min(concurrency, batch_limit * 2)
        if time.monotonic() >= deadline:
            raise ProbeError("coverage-deadline", "per-worker request target completed after its deadline",
                             responses=responses, observed=counter.coverage.counts)
        counter.coverage.verify(target, responses)
        return {"ok": True, "errors_as_success": False, "responses_checked": responses,
                "errors": 0, "timeouts": 0, "mismatches": 0,
                "warmupConcurrencyLimit": concurrency, "maximumWaveRequests": maximum_wave,
                "maximumRequestsAllowed": maximum, "coverageTimeoutSeconds": self.coverage_timeout}

    def __call__(self, *, stack, size, phase, count, ports, release_ports, probe_index, final):
        label = f"probe-{probe_index:05d}-{stack}-{size}-{phase}-{count}"
        archive = self.out / label
        archive.mkdir(parents=True)
        nonce = uuid.uuid4().hex
        target = 1 if phase == "cold" else self.warm_requests
        tree, cleanup, result, failure = None, None, None, None
        network = None
        if stack == "workerd":
            started = time.monotonic()
            network = dict(method="linux-conntrack-headroom-v1", requiredConnections=count * (target + 1),
                           startupTimeoutSeconds=self.startup_timeout, startMonotonic=started,
                           deadlineMonotonic=started + self.startup_timeout)
            write_json(archive / "network-startup.json", network)
        with tempfile.TemporaryDirectory(prefix="capsid-density-") as temp:
            folder = Path(temp)
            argv = self.prepare(folder, stack, size, count, ports, nonce)
            write_json(archive / "launch.json", {"command": ["taskset", "-c", self.cpuset, *argv],
                       "stack": stack, "size": size, "count": count, "phase": phase,
                       "ports": ports, "nonce": nonce, "mode": "managed" if stack == "capsid" else "workerd",
                       "configuredParallelRequests": self.parallel,
                       "warmupConcurrencyLimit": self.warmup_concurrency(count)})
            with (archive / "stderr.log").open("wb") as logs, (archive / "requests.jsonl").open("w", encoding="utf-8") as requests:
                try:
                    if network is not None:
                        with (archive / "network-headroom.jsonl").open("w", encoding="utf-8") as evidence:
                            network.update(wait_network_headroom(network["requiredConnections"],
                                                                 network["deadlineMonotonic"], evidence))
                        network["launchStartedMonotonic"] = time.monotonic()
                        write_json(archive / "network-startup.json", network)
                    release_ports()
                    tree = OwnedProcess(["taskset", "-c", self.cpuset, *argv], stdout=logs)
                    readiness = self.wait_ready(tree, folder, stack, ports, control_log=archive / "stderr.log",
                                                deadline=network["deadlineMonotonic"] if network is not None else None)
                    if network is not None:
                        network["readyMonotonic"] = time.monotonic()
                        write_json(archive / "network-startup.json", network)
                        if network["readyMonotonic"] >= network["deadlineMonotonic"]:
                            raise ProbeError("readiness-timeout", "network and service startup exceeded their shared deadline")
                    members = tree.pin_members()
                    if len(members) < (count + 1 if stack == "capsid" else 1):
                        raise ProbeError("pool-incomplete", "not all managed worker processes are observable")
                    coverage = Coverage(count, nonce)
                    counter = CounterLog(archive / "stderr.log", coverage)
                    correctness = self.cover(tree, counter, stack, ports, count, target, nonce, requests)
                    time.sleep(self.settle_seconds)
                    if not tree.listeners_owned(ports):
                        raise ProbeError("process-exited", "listener disappeared before PSS sample")
                    sampled = stable_pss(tree.sample, interval=self.pss_interval, attempts=self.pss_attempts)
                    result = {"stack": stack, "size": size, "phase": phase, "count": count, "status": "ok",
                              "unit": "workers" if stack == "capsid" else "cells", "finalVerification": final,
                              "pss_readings_kb": sampled["pss_readings_kb"], "pssSamples": sampled["samples"],
                              "readyProcesses": [asdict(p) for p in members.values()], "readiness": readiness,
                              "requestsPerWorker": coverage.counts, "correctness": correctness,
                              "probeDirectory": label}
                    if network is not None:
                        result["startupNetworkReadiness"] = network
                except BaseException as exc:
                    failure = exc
                finally:
                    if tree is not None:
                        try:
                            cleanup = tree.close()
                        except BaseException as exc:
                            cleanup = {"status": "error", "message": str(exc)}
                            failure = ProbeError("cleanup", "density probe cleanup failed", primary=str(failure), cleanup=str(exc))
                        write_json(archive / "cleanup.json", cleanup)
            # Keep actual launch/fixture/state bytes; do not copy runtime Unix
            # sockets, and never write generated configs into the shared APPS.
            for path in folder.rglob("*"):
                if path.is_file():
                    target_file = archive / "runtime" / path.relative_to(folder)
                    target_file.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target_file)
        if failure is not None:
            if not isinstance(failure, (ProbeError, KeyboardInterrupt, SystemExit)):
                failure = ProbeError("probe-failed", f"density probe failed: {failure}", type=type(failure).__name__)
            if isinstance(failure, ProbeError):
                write_json(archive / "result.json", dict(failure.record(), cleanup=cleanup))
            raise failure
        result["cleanup"] = cleanup
        write_json(archive / "result.json", result)
        return result

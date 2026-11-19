"""Actual five-stack pure-JS QPS runs, with isolated fresh trickle and serving trees."""

from contextlib import ExitStack
from dataclasses import asdict
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
from urllib.parse import quote

from density_probe import (
    admin_listener_ready,
    admin_request,
    deployment_operation_id,
    deployment_operation_ready,
    write_json,
)
from measurement_runtime import OwnedGroup, ProbeError, ProcFS, stable_pss
from qps_control import checked_round, expected_body
from perf_evidence import read_json, object_pairs


def exact_request(port, workload, timeout=5):
    body = expected_body(workload)
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    started = time.monotonic()
    try:
        connection.request(
            "GET", "/@capsid/orders/bench/" + workload, headers={"Connection": "close"}
        )
        response = connection.getresponse()
        received = response.read(len(body) + 1)
        if response.status != 200 or received != body:
            raise ProbeError(
                "qps-response", "wrong exact body or response status", status=response.status
            )
        return {
            "ok": True,
            "status": 200,
            "bodySHA256": hashlib.sha256(received).hexdigest(),
            "latency_ms": (time.monotonic() - started) * 1000,
        }
    finally:
        connection.close()


class QpsProbe:
    def __init__(
        self,
        *,
        binaries,
        apps,
        out,
        cpuset,
        workloads,
        rounds=5,
        warmup=5,
        duration=15,
        connections=64,
        inflight=64,
        trickle_seconds=15,
        trickle_rate=10,
        startup_timeout=120,
        request_timeout_ms=30000,
        settle_seconds=1,
        pss_interval=1,
    ):
        self.binaries, self.apps, self.out, self.cpuset = binaries, Path(apps), Path(out), cpuset
        self.workloads, self.rounds, self.warmup, self.duration = (
            workloads,
            rounds,
            warmup,
            duration,
        )
        self.connections, self.inflight = connections, inflight
        self.trickle_seconds, self.trickle_rate = trickle_seconds, trickle_rate
        self.startup_timeout, self.request_timeout_ms = startup_timeout, request_timeout_ms
        self.settle_seconds, self.pss_interval = settle_seconds, pss_interval

    def prepare(self, folder, stack, ports):
        """Return independent (root role, child role, argv, owned ports) entries."""
        if stack == "capsid":
            app = folder / "apps/orders/v1"
            for directory in (
                folder / "apps",
                app.parent,
                app,
                folder / "state",
                folder / "secrets",
                folder / "secrets/orders",
                folder / "run",
            ):
                directory.mkdir(parents=True, exist_ok=True, mode=0o700)
                directory.chmod(0o700)
            shutil.copyfile(self.apps / "qps-worker.mjs", app / "bundle.mjs")
            write_json(
                app / "capsid.json",
                {
                    "apiVersion": "capsid/app-v1",
                    "entry": "bundle.mjs",
                    "pool": {"minReady": 1, "maxWorkers": 1},
                },
            )
            write_json(
                folder / "host.json",
                {
                    "apiVersion": "capsid/host-v1",
                    "applicationsRoot": str(folder / "apps"),
                    "stateRoot": str(folder / "state"),
                    "secretRootTemplate": str(folder / "secrets/{application}"),
                    "admin": {"unix": str(folder / "run/admin.sock")},
                    "capacity": {"workersTotal": 1, "startupsConcurrent": 1},
                    "listeners": [
                        {
                            "name": "qps",
                            "tcp": f"127.0.0.1:{ports[0]}",
                            "publicScheme": "http",
                            "publicAuthority": "bench.invalid",
                            "trusted": False,
                            "routing": {"mode": "path", "suffix": ""},
                        }
                    ],
                },
            )
            for file in (app / "bundle.mjs", app / "capsid.json", folder / "host.json"):
                file.chmod(0o600)
            return [
                (
                    "host",
                    "worker",
                    [
                        str(self.binaries["capsid-host"]),
                        "--mode",
                        "managed",
                        "--host-config",
                        str(folder / "host.json"),
                        "--worker",
                        str(self.binaries["capsid-worker"]),
                    ],
                    ports,
                )
            ]
        if stack == "workerd":
            shutil.copyfile(self.apps / "qps-worker.mjs", folder / "bundle.mjs")
            (folder / "workerd.capnp").write_text(
                'using Workerd = import "/workerd/workerd.capnp";\n'
                'const worker :Workerd.Worker = (compatibilityDate = "2026-08-20", '
                'modules = [(name = "main", esModule = embed "bundle.mjs")]);\n'
                'const config :Workerd.Config = (services = [(name = "bench", worker = .worker)], '
                f'sockets = [(name = "http", address = "127.0.0.1:{ports[0]}", http = (), service = "bench")]);\n'
            )
            return [
                (
                    "process",
                    "process",
                    [str(self.binaries["workerd"]), "serve", str(folder / "workerd.capnp")],
                    ports,
                )
            ]
        if stack in ("node", "node-jitless"):
            script = folder / "qps-node.mjs"
            shutil.copyfile(self.apps / "qps-node.mjs", script)
            argv = [str(self.binaries["node"])]
            if stack == "node-jitless":
                argv.append("--jitless")
            argv += [str(script), str(ports[0])]
            return [("process", "process", argv, ports)]
        if stack == "bun":
            script = folder / "qps-bun.mjs"
            shutil.copyfile(self.apps / "qps-bun.mjs", script)
            argv = [str(self.binaries["bun"]), str(script), str(ports[0])]
            return [("process", "process", argv, ports)]
        raise ProbeError("qps-stack", "unknown QPS stack")

    def wait_ready(self, group, roots, folder, stack, port):
        deadline, deployed = time.monotonic() + self.startup_timeout, stack != "capsid"
        operation_id = None
        while time.monotonic() < deadline:
            if any(not tree.alive() for _, _, tree, _ in roots):
                raise ProbeError("qps-startup", "serving root exited before readiness")
            if not deployed:
                admin = folder / "run/admin.sock"
                if admin_listener_ready(admin, deadline):
                    operation_id = deployment_operation_id(
                        admin_request(
                            admin,
                            "POST",
                            "/v1/deploy",
                            b'{"app":"orders","version":"v1"}',
                            accepted=(202,),
                        )
                    )
                    deployed = True
            if deployed:
                if stack == "capsid":
                    if not deployment_operation_ready(folder / "run/admin.sock", operation_id):
                        time.sleep(0.02)
                        continue
                    state = json.loads(
                        admin_request(folder / "run/admin.sock", "GET", "/v1/apps/orders")
                    )
                    if state.get("active") is not True:
                        time.sleep(0.02)
                        continue
                ready = True
                for role, _, tree, ports in roots:
                    members = tree.discover()
                    minimum = 2 if role == "host" else 1
                    if len(members) < minimum or not tree.listeners_owned(ports):
                        ready = False
                if ready:
                    return exact_request(
                        port,
                        "matrix-json-1k",
                        timeout=max(
                            0.001, min(self.request_timeout_ms / 1000, deadline - time.monotonic())
                        ),
                    )
            time.sleep(0.02)
        raise ProbeError("qps-startup", "serving forest did not become ready before its deadline")

    def memory(self, group, roots):
        sampled = stable_pss(
            lambda: group.sample([tree for _, _, tree, _ in roots]),
            interval=self.pss_interval,
            attempts=8,
        )
        members, root_records = [], []
        for role, child_role, tree, _ in roots:
            root_records.append(dict(asdict(tree.root), role=role, childRole=child_role))
            members += [
                dict(asdict(p), role=role if p.pid == tree.root.pid else child_role)
                for p in tree.discover().values()
            ]
        parts = sampled["samples"][-1]["processes"]
        by_identity = {(p["pid"], p["start"]): p["role"] for p in members}
        roles = {}
        for part in parts:
            role = by_identity[(part["pid"], part["start"])]
            roles[role] = roles.get(role, 0) + part["pss_kb"]
        return dict(
            method="recursive-process-tree-pss",
            pss_kb=sampled["pss_readings_kb"][-1],
            roles_kb=roles,
            pssSamples=sampled["samples"],
            pss_readings_kb=sampled["pss_readings_kb"],
            measurementProcesses=members,
            measurementRoots=root_records,
            probeProcess=asdict(ProcFS().identity(os.getpid())),
        )

    def __call__(self, *, stack, phase, ports, release_ports):
        if phase not in ("lowLoad", "serving"):
            raise ProbeError("qps-phase", "unknown QPS phase")
        archive = self.out / "probes" / stack / phase
        archive.mkdir(parents=True, exist_ok=False)
        started, failure, result, cleanup = time.monotonic(), None, None, None
        with (
            tempfile.TemporaryDirectory(prefix="capsid-qps-", dir="/tmp") as temporary,
            ExitStack() as logs,
        ):
            folder, roots, clients = Path(temporary), [], []
            (folder / "run").mkdir(mode=0o700)
            plan = self.prepare(folder, stack, ports)
            write_json(
                archive / "launch.json",
                {
                    "stack": stack,
                    "phase": phase,
                    "ports": ports,
                    "commands": [argv for _, _, argv, _ in plan],
                    "runtimeFlags": ["--jitless"] if stack == "node-jitless" else [],
                    "cpuset": self.cpuset,
                    "mode": "managed-strict" if stack == "capsid" else stack,
                },
            )
            group = OwnedGroup()
            try:
                release_ports()
                for role, child_role, argv, owned_ports in plan:
                    output = logs.enter_context((archive / (role + ".log")).open("wb"))
                    env = dict(os.environ)
                    env.pop("NODE_OPTIONS", None)
                    env["XDG_RUNTIME_DIR"] = str(folder / "run")
                    tree = group.spawn(
                        ["taskset", "-c", self.cpuset, *argv], stdout=output, cwd=folder, env=env
                    )
                    roots.append((role, child_role, tree, owned_ports))
                readiness = self.wait_ready(group, roots, folder, stack, ports[0])
                write_json(archive / "readiness.json", readiness)
                for _, _, tree, _ in roots:
                    tree.pin_members()
                if phase == "lowLoad":
                    requests = []
                    request_log = logs.enter_context(
                        (archive / "requests.jsonl").open("x", encoding="utf-8")
                    )
                    origin = time.monotonic()
                    for index in range(self.trickle_seconds * self.trickle_rate):
                        planned = origin + index / self.trickle_rate
                        time.sleep(max(0, planned - time.monotonic()))
                        dispatched = time.monotonic()
                        if dispatched - planned > 0.1:
                            raise ProbeError("qps-trickle", "trickle schedule fell behind")
                        if any(not tree.alive() for _, _, tree, _ in roots):
                            raise ProbeError("qps-exit", "serving root exited during trickle")
                        row = exact_request(
                            ports[0],
                            "matrix-json-1k",
                            timeout=min(5, self.request_timeout_ms / 1000),
                        )
                        recorded = dict(
                            row,
                            sequence=index + 1,
                            scheduled_s=index / self.trickle_rate,
                            started_s=dispatched - origin,
                            completed_s=time.monotonic() - origin,
                        )
                        requests.append(recorded)
                        request_log.write(json.dumps(recorded) + "\n")
                        request_log.flush()
                    time.sleep(max(0, origin + self.trickle_seconds - time.monotonic()))
                    elapsed = time.monotonic() - origin
                    extra = {
                        "beforeMeasuredLoad": True,
                        "validatedRequests": len(requests),
                        "duration_s": elapsed,
                        "requests": requests,
                        "workload": "matrix-json-1k",
                    }
                else:
                    records = []
                    for workload in self.workloads:
                        for round_no in range(1, self.rounds + 1):
                            stem = f"{workload}.r{round_no}"
                            sample, verdict = (
                                archive / (stem + ".jsonl"),
                                archive / (stem + ".correctness.json"),
                            )
                            env = {
                                k: v
                                for k, v in os.environ.items()
                                if not k.startswith("CAPSID_BENCH_")
                            }
                            env.update(
                                CAPSID_BENCH_TARGET=f"http://127.0.0.1:{ports[0]}",
                                CAPSID_BENCH_WORKLOAD=workload,
                                CAPSID_BENCH_SIDE=stack,
                                CAPSID_BENCH_ROUND=str(round_no),
                                CAPSID_BENCH_WARMUP_S=str(self.warmup),
                                CAPSID_BENCH_DURATION_S=str(self.duration),
                                CAPSID_BENCH_CONNECTIONS=str(self.connections),
                                CAPSID_BENCH_INFLIGHT=str(self.inflight),
                                CAPSID_BENCH_TIMEOUT_MS=str(self.request_timeout_ms),
                                CAPSID_BENCH_SAMPLES_OUT=str(sample),
                                CAPSID_BENCH_CORRECTNESS_OUT=str(verdict),
                            )
                            output = logs.enter_context((archive / (stem + ".log")).open("wb"))
                            client = group.spawn(
                                ["taskset", "-c", self.cpuset, str(self.binaries["loadgen"])],
                                stdout=output,
                                env=env,
                            )
                            clients.append(asdict(client.root))
                            deadline = (
                                time.monotonic()
                                + self.warmup
                                + self.duration
                                + self.request_timeout_ms / 1000
                                + 10
                            )
                            while client.alive():
                                for _, _, tree, _ in roots:
                                    tree.discover()
                                    if not tree.alive():
                                        raise ProbeError(
                                            "qps-exit",
                                            "serving root exited during measured traffic",
                                        )
                                client.discover()
                                if time.monotonic() >= deadline:
                                    raise ProbeError(
                                        "qps-client", "load generator exceeded its deadline"
                                    )
                                time.sleep(0.01)
                            client.close()
                            if client.process.returncode != 0:
                                raise ProbeError(
                                    "qps-client",
                                    "load generator failed",
                                    returncode=client.process.returncode,
                                )
                            raw = [
                                json.loads(line, object_pairs_hook=object_pairs)
                                for line in sample.read_text().splitlines()
                            ]
                            correct = read_json(verdict)
                            checked_round(
                                raw,
                                correct,
                                stack,
                                workload,
                                round_no,
                                warmup=self.warmup,
                                duration=self.duration,
                                connections=self.connections,
                                inflight=self.inflight,
                                request_timeout_ms=self.request_timeout_ms,
                            )
                            records.append(
                                {
                                    "workload": workload,
                                    "round": round_no,
                                    "samples": sample.relative_to(self.out).as_posix(),
                                    "correctness": verdict.relative_to(self.out).as_posix(),
                                    "measurement": raw[1],
                                    "clientProcess": asdict(client.root),
                                }
                            )
                    extra = {"records": records}
                time.sleep(self.settle_seconds)
                memory = self.memory(group, roots)
                result = dict(
                    memory,
                    **extra,
                    stack=stack,
                    phase=phase,
                    loadgenProcesses=clients,
                    readinessResponse=readiness,
                    startedMonotonic=started,
                    completedMonotonic=time.monotonic(),
                    probeDirectory=archive.relative_to(self.out).as_posix(),
                )
            except BaseException as exc:
                failure = exc
            finally:
                try:
                    cleanup = group.close()
                except BaseException as exc:
                    failure = ProbeError(
                        "cleanup",
                        "QPS owned forest cleanup failed",
                        primary=str(failure),
                        cleanup=str(exc),
                    )
                    cleanup = {"status": "error", "message": str(exc)}
                if result is not None:
                    result["cleanupCompletedMonotonic"] = time.monotonic()
                write_json(archive / "cleanup.json", cleanup)
                runtime_files = []
                for file in folder.rglob("*"):
                    if file.is_file() and not file.is_symlink():
                        relative = file.relative_to(folder)
                        target = (
                            archive
                            / "runtime"
                            / Path(*(quote(p, safe="-._~") for p in relative.parts))
                        )
                        target.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(file, target)
                        runtime_files.append(
                            {
                                "source": relative.as_posix(),
                                "file": target.relative_to(self.out).as_posix(),
                            }
                        )
                write_json(archive / "runtime-files.json", runtime_files)
            if failure is not None:
                write_json(
                    archive / "result.json",
                    {"status": "error", "message": str(failure), "cleanup": cleanup},
                )
                raise failure
        result["cleanup"] = cleanup
        write_json(archive / "result.json", result)
        return result

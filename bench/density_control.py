"""Injectable density search and per-worker coverage, shared by real lanes/tests."""
import json
import math
from pathlib import Path
import time

from measurement_runtime import PortAllocator, ProbeError

MARKER = "CAPSID_BENCH_DENSITY_V1 "


class Coverage:
    def __init__(self, workers, nonce):
        if type(workers) is not int or workers <= 0 or not isinstance(nonce, str) or not nonce:
            raise ProbeError("coverage-protocol", "invalid worker count or probe identity")
        self.workers, self.nonce = workers, nonce
        self.counts = {}

    def add(self, record):
        if not isinstance(record, dict) or record.get("nonce") != self.nonce:
            raise ProbeError("coverage-identity", "wrong density probe identity in worker counter")
        worker, count = record.get("worker"), record.get("count")
        if not isinstance(worker, str) or not worker or type(count) is not int or count != self.counts.get(worker, 0) + 1:
            raise ProbeError("coverage-sequence", "missing, repeated or malformed per-worker counter")
        self.counts[worker] = count
        if len(self.counts) > self.workers:
            raise ProbeError("coverage-replacement", "more worker identities than the measured pool")

    def complete(self, minimum):
        return len(self.counts) == self.workers and all(n >= minimum for n in self.counts.values())

    def verify(self, minimum, responses):
        if not self.complete(minimum):
            raise ProbeError("coverage-incomplete", "not every worker reached the request target",
                             expectedWorkers=self.workers, minimum=minimum, observed=self.counts)
        if sum(self.counts.values()) != responses:
            raise ProbeError("coverage-count", "worker request counters disagree with validated responses")


def wait_network_headroom(required, deadline, output, *, read=None,
                          clock=time.monotonic, pause=time.sleep):
    """Wait on native conntrack headroom, consuming an existing startup deadline."""
    if (type(required) is not int or required <= 0 or
            type(deadline) not in (int, float) or not math.isfinite(deadline)):
        raise ProbeError("network-capacity", "invalid connection tracking prerequisite")

    if read is None:
        def read():
            values = []
            for name in ("nf_conntrack_max", "nf_conntrack_count"):
                with (Path("/proc/sys/net/netfilter") / name).open("rb") as stream:
                    raw = stream.read(64)
                if len(raw) >= 64 or not raw.strip().isdigit():
                    raise ValueError("invalid bounded kernel counter")
                values.append(int(raw))
            return values

    readings = 0
    while True:
        if clock() >= deadline:
            raise ProbeError("readiness-timeout", "startup deadline exhausted before network readiness")
        try:
            maximum, current = read()
        except (OSError, ValueError, TypeError) as exc:
            raise ProbeError("network-capacity", "native connection tracking counters unavailable or malformed") from exc
        if type(maximum) is not int or maximum <= 0 or type(current) is not int or current < 0:
            raise ProbeError("network-capacity", "invalid native connection tracking counters")
        sampled = clock()
        if sampled >= deadline:
            raise ProbeError("readiness-timeout", "startup deadline exhausted before network readiness")
        readings += 1
        eligible = maximum - current >= required
        output.write(json.dumps(dict(sequence=readings, sampledMonotonic=sampled, maximum=maximum,
                                     current=current, requiredConnections=required, eligible=eligible),
                                allow_nan=False) + "\n")
        output.flush()
        if required > maximum:
            raise ProbeError("network-capacity", "original request coverage exceeds native conntrack capacity")
        if eligible:
            return dict(readings=readings, admittedMonotonic=sampled)
        pause(max(0, min(.05, deadline - clock())))


class WorkerdReadyLog:
    """Bounded, incremental workerd control-FD events, not business requests."""
    def __init__(self, path, ports):
        if (not ports or len(set(ports)) != len(ports) or
                any(type(port) is not int or not 0 < port < 65536 for port in ports)):
            raise ProbeError("readiness-protocol", "unique positive workerd ports are required")
        self.path = Path(path)
        self.expected = {f"s{i}": port for i, port in enumerate(ports)}
        self.seen, self.offset, self.pending = {}, 0, b""
        self.maximum_bytes = max(4096, 512 * len(ports))

    def poll(self):
        def fields(pairs):
            value = {}
            for key, item in pairs:
                if key in value:
                    raise ProbeError("readiness-format", "duplicate workerd control field")
                value[key] = item
            return value

        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            data = stream.read(self.maximum_bytes - self.offset + 1)
            self.offset = stream.tell()
        if self.offset > self.maximum_bytes:
            raise ProbeError("readiness-limit", "workerd startup control log exceeds its bound")
        self.pending += data
        lines = self.pending.split(b"\n")
        self.pending = lines.pop()
        for raw in lines:
            try:
                value = json.loads(raw, object_pairs_hook=fields)
            except (ValueError, UnicodeError) as exc:
                raise ProbeError("readiness-format", "malformed workerd control event") from exc
            if (not isinstance(value, dict) or set(value) != {"event", "socket", "port"} or
                    value["event"] != "listen" or not isinstance(value["socket"], str) or
                    value["socket"] not in self.expected or type(value["port"]) is not int or
                    value["port"] != self.expected[value["socket"]] or value["socket"] in self.seen):
                raise ProbeError("readiness-identity", "unexpected, repeated or wrong-port workerd listen event")
            self.seen[value["socket"]] = value["port"]
        return not self.pending and self.seen == self.expected


class CounterLog:
    def __init__(self, path, coverage):
        self.path, self.coverage = path, coverage
        self.offset, self.pending = 0, b""

    def poll(self):
        with self.path.open("rb") as stream:
            stream.seek(self.offset)
            data = stream.read()
            self.offset = stream.tell()
        self.pending += data
        lines = self.pending.split(b"\n")
        self.pending = lines.pop()
        for raw in lines:
            line = raw.decode("utf-8", errors="strict")
            # Capsid's normal application log is a structured JSON object;
            # workerd prints the console message with an optional prefix.
            try:
                outer = json.loads(line)
            except ValueError:
                outer = None
            message = outer.get("message", "") if isinstance(outer, dict) else line
            if not isinstance(message, str) or MARKER not in message:
                continue
            payload = message.split(MARKER, 1)[1]
            try:
                record, end = json.JSONDecoder().raw_decode(payload)
            except ValueError as exc:
                raise ProbeError("coverage-format", "malformed benchmark worker counter") from exc
            if payload[end:].strip():
                raise ProbeError("coverage-format", "unexpected suffix after benchmark worker counter")
            self.coverage.add(record)


def checked_probe(result, stack, size, phase, count, warm_requests):
    required = {"stack": stack, "size": size, "phase": phase, "count": count,
                "unit": "workers" if stack == "capsid" else "cells", "status": "ok"}
    if (not isinstance(result, dict) or type(result.get("count")) is not int or
            any(result.get(k) != v for k, v in required.items())):
        raise ProbeError("probe-identity", "density probe returned the wrong cell or failed status")
    readings = result.get("pss_readings_kb")
    if (not isinstance(readings, list) or len(readings) != 2 or
            any(type(v) is not int or v <= 0 for v in readings) or readings[0] != readings[1]):
        raise ProbeError("pss-unstable", "probe requires two equal positive PSS readings")
    coverage = result.get("requestsPerWorker")
    minimum = 1 if phase == "cold" else warm_requests
    if (not isinstance(coverage, dict) or len(coverage) != count or
            any(not isinstance(k, str) or not k or type(v) is not int or v < minimum for k, v in coverage.items())):
        raise ProbeError("coverage-incomplete", "probe has not proved coverage of every worker")
    correctness = result.get("correctness")
    if (not isinstance(correctness, dict) or correctness.get("ok") is not True or
            correctness.get("errors_as_success") is not False or
            any(type(correctness.get(k)) is not int or correctness[k] != 0 for k in ("errors", "timeouts", "mismatches")) or
            type(correctness.get("responses_checked")) is not int or
            correctness["responses_checked"] != sum(coverage.values())):
        raise ProbeError("correctness", "missing or inconsistent exact-response correctness record")
    cleanup = result.get("cleanup")
    if not isinstance(cleanup, dict) or type(cleanup.get("ownedPidsRemaining")) is not int or cleanup["ownedPidsRemaining"] != 0:
        raise ProbeError("cleanup", "probe did not clean up every owned process")
    return readings[0]


class DensitySearch:
    def __init__(self, probe, sink, *, allocator=None, budget_mb=2048, warm_requests=100,
                 final_retries=2, final_steps=8, deadline_seconds=3600, clock=time.monotonic):
        self.probe, self.sink = probe, sink
        if (type(budget_mb) not in (int, float) or not math.isfinite(budget_mb) or budget_mb <= 0 or
                type(warm_requests) is not int or warm_requests <= 0 or
                type(final_retries) is not int or final_retries < 0 or
                type(final_steps) is not int or final_steps < 0 or
                type(deadline_seconds) not in (int, float) or not math.isfinite(deadline_seconds) or deadline_seconds <= 0):
            raise ProbeError("search-protocol", "invalid density search limits")
        self.allocator = allocator or PortAllocator()
        self.budget_kb = budget_mb * 1024
        self.warm_requests = warm_requests
        self.final_retries, self.final_steps = final_retries, final_steps
        self.clock, self.deadline = clock, clock() + deadline_seconds
        self.index = 0

    def attempt(self, stack, size, phase, count, final):
        self.index += 1
        record = {"probeIndex": self.index, "stack": stack, "size": size, "phase": phase,
                  "count": count, "finalVerification": final}
        try:
            if self.clock() >= self.deadline:
                raise ProbeError("search-timeout", "density search exceeded its deadline")
            # This state lives in the control layer. Capturing a probe's stdout
            # never captures the port allocator or the cleanup owner.
            with self.allocator.allocate(1 if stack == "capsid" else count) as lease:
                record["ports"] = list(lease.ports)
                result = self.probe(stack=stack, size=size, phase=phase, count=count,
                                    ports=list(lease.ports), release_ports=lease.release,
                                    probe_index=self.index, final=final)
            pss = checked_probe(result, stack, size, phase, count, self.warm_requests)
            record.update(result)
            record["finalVerification"] = final
            self.sink(record)
            return record, pss <= self.budget_kb
        except ProbeError as exc:
            record.update(exc.record())
            self.sink(record)
            raise
        except Exception as exc:
            error = ProbeError("probe-failed", f"density probe failed: {exc}", type=type(exc).__name__)
            record.update(error.record())
            self.sink(record)
            raise error from exc

    def fill(self, stack, size, phase, cap):
        if stack not in ("capsid", "workerd") or phase not in ("cold", "warm") or type(cap) is not int or cap <= 0:
            raise ProbeError("search-protocol", "invalid density cell or cap")
        low, high, n = 0, None, 1
        while True:
            _, fits = self.attempt(stack, size, phase, n, False)
            if not fits:
                high = n
                break
            low = n
            if low == cap:
                break
            n = min(n * 2, cap)
        if low == 0:
            raise ProbeError("no-pool-fit", "one verified worker/cell already exceeds the budget")
        if high is not None:
            while high - low > 1:
                mid = (low + high) // 2
                _, fits = self.attempt(stack, size, phase, mid, False)
                if fits:
                    low = mid
                else:
                    high = mid
        # Always verify, including count=1 and a cap reached without finding
        # the budget. Only a new successful under-budget probe can be a cell.
        for _ in range(self.final_steps + 1):
            last_error = None
            for retry in range(self.final_retries + 1):
                try:
                    result, fits = self.attempt(stack, size, phase, low, True)
                    last_error = None
                    break
                except ProbeError as exc:
                    # Only bounded PSS drift is retryable. Wrong content,
                    # unreadable/nonpositive PSS or a dead pool aborts the run.
                    if exc.code != "pss-unstable":
                        raise
                    last_error = exc
            if last_error is not None:
                raise ProbeError("final-verification", "final PSS did not stabilize within its retry limit") from last_error
            if fits:
                result["limit"] = "cap" if low == cap else "budget"
                return result
            low -= 1
            if low <= 0:
                break
        raise ProbeError("final-verification", "no under-budget final verification within the step-down limit")

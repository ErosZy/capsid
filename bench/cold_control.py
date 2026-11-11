"""Pure cold-probe validation shared by producer tests and actual probes."""
import math
import re
import statistics

from measurement_runtime import ProbeError

EXPECTED_BODY = b'{"status":"ok","total":2039,"count":16}'


def cpu_selection(text, available):
    selected = set()
    if not available or not isinstance(text, str) or not text:
        raise ProbeError("cold-affinity", "missing CPU affinity")
    for item in text.split(','):
        if re.fullmatch(r'[0-9]+(?:-[0-9]+)?', item) is None:
            raise ProbeError("cold-affinity", "invalid CPU list")
        ends = [int(value) for value in item.split('-')]
        first, last = ends[0], ends[-1]
        if first > last or last > max(available):
            raise ProbeError("cold-affinity", "CPU range is outside the available affinity")
        selected.update(range(first, last + 1))
    if not selected <= set(available):
        raise ProbeError("cold-affinity", "requested CPU is unavailable")
    return selected


def number(value, field, minimum=0):
    if type(value) not in (int, float) or not math.isfinite(value) or value < minimum:
        raise ProbeError("cold-number", f"invalid {field}")
    return value


def identity(value):
    if (not isinstance(value, dict) or type(value.get("pid")) is not int or value["pid"] <= 0 or
            type(value.get("start")) is not int or value["start"] < 0):
        raise ProbeError("cold-identity", "missing PID/start identity")
    return value["pid"], value["start"]


def checked_round(row, warm_requests, *, observed=None):
    if type(warm_requests) is not int or warm_requests <= 0:
        raise ProbeError("cold-warm", "positive warm request target is required")
    if (not isinstance(row, dict) or row.get("ok") is not True or type(row.get("warm_n")) is not int or
            row["warm_n"] != warm_requests):
        raise ProbeError("cold-response", "failed response or incomplete warm requests")
    for field in ("total_ms", "ready_ms", "warm_ms"):
        number(row.get(field), field)
    if row["total_ms"] < row["ready_ms"]:
        raise ProbeError("cold-timing", "first response precedes readiness")
    verdict = row.get("correctness", {})
    if (not isinstance(verdict, dict) or verdict.get("ok") is not True or verdict.get("errors_as_success") is not False or
            type(verdict.get("responses_checked")) is not int or verdict["responses_checked"] != warm_requests + 1 or
            any(type(verdict.get(k)) is not int or verdict[k] != 0 for k in ("errors", "timeouts", "mismatches"))):
        raise ProbeError("cold-correctness", "missing exact first/warm response verdict")
    latencies = row.get("warm_latencies_ms")
    if not isinstance(latencies, list) or len(latencies) != warm_requests:
        raise ProbeError("cold-warm", "missing per-request warm latencies")
    for value in latencies:
        number(value, "warm latency")
    if abs(statistics.median(latencies) - row["warm_ms"]) > .000002:
        raise ProbeError("cold-warm", "warm median disagrees with per-request samples")
    members = row.get("measurementProcesses")
    if not isinstance(members, list) or not members:
        raise ProbeError("cold-identity", "missing pinned process members")
    identities = {identity(p) for p in members}
    if len(identities) != len(members) or identity(row.get("measurementRoot")) not in identities:
        raise ProbeError("cold-identity", "invalid or duplicate measurement members")
    probe = identity(row.get("probeProcess"))
    stack = row.get("stack")
    if stack in ("capsid-source", "capsid-bytecode"):
        if (row.get("transport") != "c-abi" or row.get("posture") != "c-abi-strict" or
                type(row.get("worker_pid")) is not int or row["worker_pid"] != row["measurementRoot"]["pid"]):
            raise ProbeError("cold-identity", "missing strict C-ABI worker identity")
    elif stack not in ("node", "deno", "workerd") or row.get("transport") != "http":
        raise ProbeError("cold-transport", "unknown cold stack or measurement transport")
    if any(pid == probe[0] for pid, _ in identities):
        raise ProbeError("cold-identity", "probe process was included in measured memory")
    if observed is not None and not identities <= set(observed):
        raise ProbeError("cold-identity", "sampled process was never observed in the owned tree")
    readings = row.get("pss_readings_kb")
    pss = row.get("pss_kb")
    if type(pss) is not int or pss <= 0 or readings != [pss, pss] or any(type(p) is not int for p in readings):
        raise ProbeError("cold-pss", "missing stable positive PSS readings")
    samples = row.get("pssSamples")
    if not isinstance(samples, list) or len(samples) != 2:
        raise ProbeError("cold-pss", "missing both recursive PSS samples")
    for sample in samples:
        parts = sample.get("processes") if isinstance(sample, dict) else None
        if not isinstance(parts, list) or {identity(p) for p in parts} != identities or len(parts) != len(identities):
            raise ProbeError("cold-identity", "PSS membership differs from pinned processes")
        if (any(type(p.get("pss_kb")) is not int or p["pss_kb"] <= 0 for p in parts) or
                type(sample.get("pss_kb")) is not int or sum(p["pss_kb"] for p in parts) != pss or sample["pss_kb"] != pss):
            raise ProbeError("cold-pss", "recursive PSS sum disagrees with stable total")
    cleanup = row.get("cleanup", {})
    if (not isinstance(cleanup, dict) or type(cleanup.get("ownedPidsRemaining")) is not int or
            cleanup["ownedPidsRemaining"] != 0):
        raise ProbeError("cold-cleanup", "owned processes remain after cold probe")
    return row

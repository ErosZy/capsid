"""Producer-side run identity and sealed lane artifacts.

Formal runs consume the clean build/session metadata prepared by the release
measurement controller. Observation runs never manufacture a lane envelope.
"""
from contextlib import contextmanager
from pathlib import Path
import datetime
import hashlib
import os
import platform
import re
import shutil
import stat
import subprocess
import sys

if sys.platform == "linux":
    import fcntl
else:
    fcntl = None

from measurement_runtime import ProbeError
from perf_evidence import read_json, relative_name, sha256


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def machine_identity():
    cpu = next((line.partition(":")[2].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.processor())
    machine_id = Path("/etc/machine-id").read_bytes().strip()
    return {"cpu": cpu, "kernel": platform.release(), "architecture": platform.machine(),
            "logicalCpus": os.cpu_count(), "hostId": hashlib.sha256(machine_id).hexdigest()}


def git(source, *args):
    run = subprocess.run(["git", "-C", str(source), *args], capture_output=True, text=True)
    if run.returncode:
        raise ProbeError("source-identity", "cannot verify measured source checkout", operation=list(args))
    return run.stdout.strip()


def run_metadata(path, binary_paths):
    metadata = read_json(path)
    if not isinstance(metadata, dict) or type(metadata.get("schema")) is not int or metadata["schema"] != 1:
        raise ProbeError("run-metadata", "missing versioned measurement metadata")
    origin = metadata.get("origin", {})
    source = Path(metadata.get("sourceRoot", ""))
    if (not isinstance(origin, dict) or not source.is_dir() or origin.get("clean") is not True or
            not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", str(origin.get("commit", ""))) or
            not isinstance(origin.get("version"), str) or not origin["version"] or
            not isinstance(origin.get("release"), str) or not origin["release"] or
            origin.get("tag") not in ("", origin.get("release"))):
        raise ProbeError("source-identity", "formal run requires a clean measured source identity")
    if git(source, "rev-parse", "HEAD") != origin["commit"] or git(source, "status", "--porcelain"):
        raise ProbeError("source-identity", "measured source checkout is dirty or has the wrong commit")
    version = re.search(r"project\(capsid_runtime\s+VERSION\s+([^\s)]+)", (source / "CMakeLists.txt").read_text())
    if version is None or version[1] != origin["version"]:
        raise ProbeError("source-identity", "measured source version disagrees with run metadata")
    if origin["tag"] and git(source, "describe", "--exact-match", "--tags", "HEAD") != origin["tag"]:
        raise ProbeError("source-identity", "measured source tag disagrees with run metadata")
    if (metadata.get("machine") != machine_identity() or not isinstance(metadata.get("session"), str) or
            not metadata["session"].strip() or metadata.get("quietSession") is not True):
        raise ProbeError("session-identity", "formal run requires the current machine and a coordinated quiet session")
    binaries = metadata.get("binaries", {})
    if not isinstance(binaries, dict):
        raise ProbeError("binary-identity", "missing measured binary identities")
    result = {}
    for name, path in binary_paths.items():
        item = binaries.get(name, {})
        if not isinstance(item, dict) or item.get("sha256") != sha256(path):
            raise ProbeError("binary-identity", f"{name} bytes disagree with measurement metadata")
        for key in ("commit", "tag", "version"):
            if not isinstance(item.get(key), str):
                raise ProbeError("binary-identity", f"{name} lacks measured {key}")
        if (not item["version"].strip() or (item["commit"] and
                not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", item["commit"])) or
                not (item["commit"] or item["tag"].strip())):
            raise ProbeError("binary-identity", f"{name} has an invalid measured source identity")
        if (name.startswith("capsid-") or name in ("cold-start", "loadgen")) and any(item[key] != origin[key] for key in ("commit", "tag", "version")):
            raise ProbeError("binary-identity", f"{name} source disagrees with measured checkout")
        result[name] = {key: item[key] for key in ("commit", "tag", "version", "sha256")}
        result[name]["measuredPath"] = str(path)
    return dict(metadata, binaries=result)


@contextmanager
def session_lock(enabled):
    if not enabled:
        yield None
        return
    if fcntl is None:
        raise ProbeError("platform", "formal performance session coordination requires Linux")
    path = Path(temp_directory()) / "capsid-performance-session.lock"
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        observed = os.fstat(descriptor)
        if not stat.S_ISREG(observed.st_mode) or observed.st_uid != os.geteuid():
            raise ProbeError("session-lock", "performance session lock is not an owned regular file")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise ProbeError("session-overlap", "another coordinated performance lane is already running") from exc
        yield {"path": str(path), "held": True}
    finally:
        os.close(descriptor)


def temp_directory():
    # Keep the shared coordination lock independent of lane-specific OUT/APPS;
    # only the lock is shared, never probe files, ports or failure records.
    return "/tmp"


def seal_archive(out):
    entries = []
    for path in sorted(out.rglob("*")):
        if path.is_symlink():
            raise ProbeError("archive-path", "measurement archive contains a symlink")
        if path.is_file() and path != out / "sha256sums.txt":
            name = path.relative_to(out).as_posix()
            if "\n" in name or "\r" in name:
                raise ProbeError("archive-path", "newline in archive file name")
            relative_name(name)
            entries.append(f"{sha256(path)}  {name}\n")
    (out / "sha256sums.txt").write_text("".join(entries), encoding="utf-8", newline="\n")


def readable_manifest(manifest):
    pairs = {"generated_at": manifest["startedAt"], "commit": manifest["origin"]["commit"],
             "tag": manifest["origin"]["tag"], "command": manifest["command"],
             "cpu": manifest["machine"]["cpu"], "uname": manifest["machine"]["kernel"],
             "nproc": str(manifest["machine"]["logicalCpus"])}
    pairs.update({"binary." + key + ".sha256": value["sha256"] for key, value in manifest["binaries"].items()})
    pairs.update({"fixture." + key + ".sha256": value["sha256"] for key, value in manifest["fixtures"].items()})
    return "".join(f"{key}: {value}\n" for key, value in pairs.items())


def snapshot_runner(out, files=None):
    root = Path(__file__).resolve().parent
    files = files or ("density-lane.sh", "density-lane.py", "density_control.py", "density_probe.py", "density_conditioning.py",
             "measurement_runtime.py", "perf_run.py", "gen-cold-start-apps.py",
             "perf_evidence.py", "density_evidence.py", "cold_control.py", "qps_control.py", "collect-perf-point.py")
    inputs, archived = {}, {}
    for name in files:
        source = root / name
        target = out / "runner" / name
        target.parent.mkdir(exist_ok=True)
        target.write_bytes(source.read_bytes())
        inputs[str(source)] = sha256(target)
        archived["runner/" + name] = inputs[str(source)]
    launcher = shutil.which("taskset")
    if launcher is None:
        raise ProbeError("launcher-missing", "performance lane requires taskset")
    tools = {}
    for name, file in (("python", Path(sys.executable).resolve()), ("taskset", Path(launcher).resolve())):
        value = sha256(file)
        tools[name] = {"path": str(file), "sha256": value}
        inputs[str(file)] = value
    tools["python"]["version"] = sys.version
    return {"files": archived, "tools": tools}, inputs


def recheck_runner(inputs):
    if any(sha256(Path(path)) != value for path, value in inputs.items()):
        raise ProbeError("runner-changed", "measurement runner or launcher bytes changed during the lane")

"""Owned benchmark processes, port leases and fail-closed Linux PSS sampling.

Nothing is launched at import time. Tests inject samplers/operations; the lane
CLI never accepts a fake sampler. Process cleanup uses observed PID/start-time
identities, never a process-name search or an unrestricted process-group kill.
"""
from dataclasses import asdict, dataclass
from pathlib import Path
import ctypes
import math
import os
import signal
import socket
import subprocess
import sys
import time


class ProbeError(RuntimeError):
    def __init__(self, code, message, **details):
        super().__init__(message)
        self.code, self.details = code, details

    def record(self):
        return {"status": "error", "error": {"code": self.code, "message": str(self), **self.details}}


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    start: int
    parent: int
    session: int
    state: str

    def same(self, other):
        return other is not None and (self.pid, self.start) == (other.pid, other.start)


class ProcFS:
    def __init__(self, root=Path("/proc")):
        self.root = Path(root)

    def identity(self, pid):
        try:
            stat = (self.root / str(pid) / "stat").read_text(errors="surrogateescape")
        except (FileNotFoundError, ProcessLookupError):
            return None
        fields = stat[stat.rfind(")") + 2:].split()
        if len(fields) < 20:
            raise ProbeError("process-stat", f"incomplete process identity for {pid}")
        return ProcessIdentity(pid, int(fields[19]), int(fields[1]), int(fields[3]), fields[0])

    def snapshot(self):
        processes = {}
        for path in self.root.iterdir():
            if not path.name.isdigit():
                continue
            try:
                identity = self.identity(int(path.name))
            except PermissionError:
                continue  # Unrelated processes may be hidden by the host.
            if identity is not None:
                processes[identity.pid] = identity
        return processes

    def pss_kb(self, identity):
        if not identity.same(self.identity(identity.pid)):
            raise ProbeError("process-changed", f"process {identity.pid} changed before PSS read")
        try:
            lines = (self.root / str(identity.pid) / "smaps_rollup").read_text().splitlines()
        except OSError as exc:
            raise ProbeError("pss-unreadable", f"cannot read PSS for {identity.pid}: {exc}") from exc
        values = [line.split() for line in lines if line.startswith("Pss:")]
        if len(values) != 1 or len(values[0]) != 3 or values[0][2] != "kB":
            raise ProbeError("pss-format", f"missing or malformed PSS for {identity.pid}")
        try:
            value = int(values[0][1])
        except ValueError as exc:
            raise ProbeError("pss-format", f"invalid PSS for {identity.pid}") from exc
        if value <= 0:
            raise ProbeError("pss-nonpositive", f"nonpositive PSS for {identity.pid}")
        if not identity.same(self.identity(identity.pid)):
            raise ProbeError("process-changed", f"process {identity.pid} changed during PSS read")
        return value


def owned_identities(root, known, snapshot):
    """Follow actual ancestry and the private session created by Popen.

    Keep the root unreaped until cleanup: its PID/session cannot be reused
    while an orphaned member is being discovered. Known identities allow a
    previously observed descendant to retain ownership after reparenting.
    """
    current_root = snapshot.get(root.pid)
    if current_root is not None and not root.same(current_root):
        raise ProbeError("pid-reused", "owned root PID was reused")
    owned = {pid: snapshot[pid] for pid, identity in known.items()
             if identity.same(snapshot.get(pid))}
    if current_root is not None:
        owned[root.pid] = current_root
    for identity in snapshot.values():
        if identity.session == root.pid and identity.start >= root.start:
            owned[identity.pid] = identity
    while True:
        added = [p for p in snapshot.values() if p.pid not in owned and p.parent in owned and
                 p.start >= owned[p.parent].start]
        if not added:
            return owned
        owned.update((p.pid, p) for p in added)


def postorder(processes, root_pid):
    def depth(identity):
        seen, count = set(), 0
        while identity.pid != root_pid and identity.parent in processes:
            if identity.pid in seen:
                raise ProbeError("process-cycle", "cycle in process ancestry")
            seen.add(identity.pid)
            count += 1
            identity = processes[identity.parent]
        return count
    return sorted(processes.values(), key=lambda p: (p.pid != root_pid, depth(p), p.pid), reverse=True)


class Subreaper:
    """A benchmark controller reaps its own orphaned descendants.

    https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html
    Restore the original process setting; never waitpid(-1) on foreign jobs.
    """
    def __init__(self):
        if sys.platform != "linux":
            raise ProbeError("platform", "process-tree PSS measurement requires Linux")
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.previous = ctypes.c_int()
        if self.libc.prctl(37, ctypes.byref(self.previous), 0, 0, 0) != 0:
            raise ProbeError("subreaper", "cannot query child subreaper state", errno=ctypes.get_errno())
        if self.libc.prctl(36, 1, 0, 0, 0) != 0:
            raise ProbeError("subreaper", "cannot enable child subreaper", errno=ctypes.get_errno())
        self.closed = False

    def close(self):
        if not self.closed:
            if self.libc.prctl(36, self.previous.value, 0, 0, 0) != 0:
                raise ProbeError("subreaper", "cannot restore child subreaper state", errno=ctypes.get_errno())
            self.closed = True


class OwnedProcess:
    _active = False
    _group = None

    def __init__(self, argv, *, stdout, stderr=None, cwd=None, env=None, proc=None, group=None):
        if group is None and OwnedProcess._active:
            raise ProbeError("ownership", "only one active process tree is allowed per controller")
        if group is not None and (group is not OwnedProcess._group or group.closed):
            raise ProbeError("ownership", "process group is not the active owner")
        self.group = group
        self.proc = proc or ProcFS()
        self.reaper = group.reaper if group is not None else Subreaper()
        self.process = None
        self.known, self.signals = {}, []
        self.required_members = None
        self.closed = False
        try:
            self.started_at = time.monotonic()
            self.process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=stdout,
                                            stderr=stderr if stderr is not None else stdout,
                                            cwd=cwd, env=env, start_new_session=True)
            self.root = self.proc.identity(self.process.pid)
            if self.root is None or self.root.session != self.process.pid:
                raise ProbeError("launch", "launched process has no private session")
            self.known[self.root.pid] = self.root
            OwnedProcess._active = True
            if group is not None:
                group.trees.append(self)
        except BaseException:
            if self.process is not None:
                # Popen already owns an unreaped child, even if an injected
                # reader or a signal interrupted identity registration. Use
                # the real process table to recover that ownership and clean
                # the complete tree before propagating the launch exception.
                self.proc = ProcFS()
                self.root = self.proc.identity(self.process.pid)
                if self.root is not None:
                    self.known[self.root.pid] = self.root
                    OwnedProcess._active = True
                    try:
                        self.close()
                    except BaseException as exc:
                        raise ProbeError("launch-cleanup", "failed to reap a partially initialized process tree") from exc
                else:
                    self.process.wait(timeout=2)
            if group is None:
                self.reaper.close()
            raise

    def discover(self):
        if self.closed:
            return {}
        found = owned_identities(self.root, self.known, self.proc.snapshot())
        self.known.update(found)
        return found

    def alive(self):
        identity = self.proc.identity(self.root.pid)
        return self.root.same(identity) and identity.state not in ("Z", "X")

    def pin_members(self):
        tree = self.discover()
        if not self.alive() or any(p.state in ("Z", "X") for p in tree.values()):
            raise ProbeError("process-exited", "process exited before measurement membership was pinned")
        self.required_members = frozenset((p.pid, p.start) for p in tree.values())
        return tree

    def sample(self):
        tree = self.discover()
        if not self.alive() or any(p.state in ("Z", "X") for p in tree.values()):
            raise ProbeError("process-exited", "process tree changed or exited before PSS sample")
        current_members = frozenset((p.pid, p.start) for p in tree.values())
        if self.required_members is None:
            self.required_members = current_members
        elif current_members != self.required_members:
            raise ProbeError("process-changed", "measured process membership changed after readiness",
                             expected=sorted(self.required_members), observed=sorted(current_members))
        parts = [{"pid": p.pid, "start": p.start, "pss_kb": self.proc.pss_kb(p)}
                 for p in sorted(tree.values(), key=lambda p: p.pid)]
        after = self.discover()
        before_ids = {(p.pid, p.start) for p in tree.values()}
        after_ids = {(p.pid, p.start) for p in after.values()}
        if before_ids != after_ids or any(p.state in ("Z", "X") for p in after.values()):
            raise ProbeError("process-changed", "process tree changed during PSS sample")
        return {"pss_kb": sum(part["pss_kb"] for part in parts), "processes": parts}

    def listeners_owned(self, ports):
        expected = {port: set() for port in ports}
        for line in (self.proc.root / "net/tcp").read_text().splitlines()[1:]:
            fields = line.split()
            if len(fields) > 9 and fields[3] == "0A":
                address, port = fields[1].split(":")
                port = int(port, 16)
                if port in expected and address in ("0100007F", "00000000"):
                    expected[port].add(fields[9])
        if any(not values for values in expected.values()):
            return False
        inodes = set()
        for identity in self.discover().values():
            folder = self.proc.root / str(identity.pid) / "fd"
            try:
                descriptors = list(folder.iterdir())
            except (FileNotFoundError, PermissionError):
                continue
            for descriptor in descriptors:
                try:
                    target = os.readlink(descriptor)
                except (FileNotFoundError, PermissionError):
                    continue
                if target.startswith("socket:[") and target.endswith("]"):
                    inodes.add(target[8:-1])
        if any(not values <= inodes for values in expected.values()):
            raise ProbeError("foreign-listener", "serving port belongs to an unrelated process")
        return True

    def _signal(self, identity, sig):
        if not identity.same(self.proc.identity(identity.pid)):
            return
        try:
            os.kill(identity.pid, sig)
        except ProcessLookupError:
            return
        self.signals.append({"pid": identity.pid, "start": identity.start, "signal": sig.name})

    def _reap(self):
        for identity in self.known.values():
            if identity.pid == self.root.pid or not identity.same(self.proc.identity(identity.pid)):
                continue
            try:
                os.waitpid(identity.pid, os.WNOHANG)
            except ChildProcessError:
                pass  # A living owned parent still owns this child.

    def close(self, timeout=5):
        if self.closed:
            return {"ownedPidsRemaining": 0, "signals": self.signals}
        deadline = time.monotonic() + timeout
        failure = None
        try:
            # Freeze the owned tree before teardown. Managed supervisors must
            # not create replacement workers while their children are removed.
            frozen = set()
            while True:
                tree = self.discover()
                pending = [p for p in reversed(postorder(tree, self.root.pid))
                           if p.state not in ("Z", "X") and (p.pid, p.start) not in frozen]
                if not pending and all(p.state in ("T", "t", "Z", "X") for p in tree.values()):
                    break
                if time.monotonic() >= deadline:
                    failure = ProbeError("cleanup-timeout", "process tree did not quiesce for cleanup")
                    break
                for identity in pending:
                    self._signal(identity, signal.SIGSTOP)
                    frozen.add((identity.pid, identity.start))
                time.sleep(.005)
            for identity in postorder(self.discover(), self.root.pid):
                self._signal(identity, signal.SIGKILL)
            self.process.wait(timeout=max(.01, deadline - time.monotonic()))
            while True:
                self._reap()
                remaining = [p for p in self.known.values() if p.same(self.proc.identity(p.pid))]
                if not remaining:
                    break
                if time.monotonic() >= deadline:
                    raise ProbeError("cleanup-timeout", "owned descendants were not reaped",
                                     remaining=[asdict(p) for p in remaining])
                time.sleep(.01)
            self.closed = True
            if failure is not None:
                raise failure
            return {"ownedPidsRemaining": 0, "signals": self.signals,
                    "processes": [asdict(p) for p in self.known.values()]}
        except BaseException:
            # A sampling/cancellation/cleanup error still owns its children.
            # Make a bounded best effort using only pinned identities before
            # propagating the original failure. Never report success here.
            try:
                self.discover()
            except (OSError, ProbeError):
                pass
            for identity in postorder(self.known, self.root.pid):
                self._signal(identity, signal.SIGKILL)
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            for _ in range(100):
                self._reap()
                if not any(p.same(self.proc.identity(p.pid)) for p in self.known.values()):
                    self.closed = True
                    break
                time.sleep(.01)
            raise
        finally:
            if self.closed and self.group is None:
                OwnedProcess._active = False
                self.reaper.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()


class OwnedGroup:
    """One subreaper lease for several independently owned process trees.

    Serving roots and transient load generators keep distinct sessions. Only
    explicitly selected serving trees enter memory samples. Closing a client
    cannot restore the reaper while a serving tree still needs it.
    """
    def __init__(self):
        if OwnedProcess._active:
            raise ProbeError("ownership", "another benchmark process owner is already active")
        self.reaper = Subreaper()
        self.trees, self.failures = [], []
        self.closed = False
        OwnedProcess._active, OwnedProcess._group = True, self

    def spawn(self, argv, **kwargs):
        return OwnedProcess(argv, group=self, **kwargs)

    def _selected(self, trees):
        selected = list(trees)
        if (self.closed or not selected or len({id(t) for t in selected}) != len(selected) or
                any(t not in self.trees or t.closed for t in selected)):
            raise ProbeError("ownership", "invalid or closed measured process tree")
        return selected

    def sample(self, trees):
        selected = self._selected(trees)
        parts = [part for tree in selected for part in tree.sample()["processes"]]
        ids = {(p["pid"], p["start"]) for p in parts}
        if len(ids) != len(parts):
            raise ProbeError("ownership", "process was included in more than one measured tree")
        # Recheck the first roots after reading the last root's PSS too.
        for tree in selected:
            current = tree.discover()
            members = frozenset((p.pid, p.start) for p in current.values())
            if not tree.alive() or members != tree.required_members or any(p.state in ("Z", "X") for p in current.values()):
                raise ProbeError("process-changed", "serving forest changed during PSS sample")
        return {"pss_kb": sum(p["pss_kb"] for p in parts), "processes": sorted(parts, key=lambda p: p["pid"])}

    def close(self):
        if not self.closed:
            for tree in reversed(self.trees):
                try:
                    tree.close()
                except BaseException as exc:
                    self.failures.append(str(exc))
            if any(not tree.closed for tree in self.trees):
                raise ProbeError("cleanup", "owned process forest was not fully reaped", failures=self.failures)
            # Restore only after all roots and adopted descendants are gone.
            self.reaper.close()
            self.closed = True
            OwnedProcess._active, OwnedProcess._group = False, None
        if self.failures:
            raise ProbeError("cleanup", "owned process forest cleanup failed", failures=self.failures)
        return {"ownedPidsRemaining": 0, "trees": [{"root": asdict(t.root), "signals": t.signals,
                "processes": [asdict(p) for p in t.known.values()], "returncode": t.process.returncode}
                for t in self.trees]}

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class PortLease:
    def __init__(self, sockets):
        self.sockets = sockets
        self.ports = [sock.getsockname()[1] for sock in sockets]

    def release(self):
        for sock in self.sockets:
            sock.close()
        self.sockets = []

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.release()


class PortAllocator:
    def __init__(self):
        self.previous = set()

    def allocate(self, count, requested=None):
        if type(count) is not int or count <= 0 or requested is not None and len(requested) != count:
            raise ProbeError("port-count", "invalid requested port count")
        sockets = []
        try:
            for index in range(count):
                for attempt in range(1000):
                    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    try:
                        sock.bind(("127.0.0.1", requested[index] if requested is not None else 0))
                    except OSError as exc:
                        sock.close()
                        raise ProbeError("port-in-use", f"cannot reserve serving port: {exc}") from exc
                    if sock.getsockname()[1] not in self.previous:
                        sockets.append(sock)
                        break
                    sock.close()
                    if requested is not None:
                        raise ProbeError("port-reused", "consecutive probes must use different ports")
                else:
                    raise ProbeError("port-exhausted", "cannot reserve fresh probe ports")
            lease = PortLease(sockets)
            self.previous = set(lease.ports)
            return lease
        except BaseException:
            for sock in sockets:
                sock.close()
            raise


def stable_pss(sample, *, interval=1, attempts=8, sleep=time.sleep):
    if type(attempts) is not int or attempts < 2:
        raise ProbeError("pss-protocol", "at least two readings are required")
    if type(interval) not in (int, float) or not math.isfinite(interval) or interval < 0:
        raise ProbeError("pss-protocol", "invalid PSS sampling interval")
    readings = []
    for index in range(attempts):
        try:
            current = sample()
        except OSError as exc:
            raise ProbeError("pss-unreadable", f"PSS sampler failed: {exc}") from exc
        if not isinstance(current, dict):
            raise ProbeError("pss-format", "PSS sampler returned no process evidence")
        value = current.get("pss_kb")
        parts = current.get("processes")
        if type(value) is not int or value <= 0 or not isinstance(parts, list) or not parts:
            raise ProbeError("pss-nonpositive", "PSS must be a positive integer with process evidence")
        if any(not isinstance(p, dict) or type(p.get("pid")) is not int or p["pid"] <= 0 or
               type(p.get("start")) is not int or p["start"] < 0 for p in parts):
            raise ProbeError("pss-format", "invalid sampled process identity")
        ids = {(p["pid"], p["start"]) for p in parts}
        if len(ids) != len(parts) or any(type(p.get("pss_kb")) is not int or p["pss_kb"] <= 0 for p in parts):
            raise ProbeError("pss-format", "invalid process PSS evidence")
        if sum(p["pss_kb"] for p in parts) != value:
            raise ProbeError("pss-format", "process PSS sum disagrees with total")
        readings.append(current)
        if len(readings) > 1:
            previous = readings[-2]
            previous_ids = {(p["pid"], p["start"]) for p in previous["processes"]}
            if previous_ids != ids:
                raise ProbeError("process-changed", "process tree changed between PSS readings")
            if previous["pss_kb"] == value:
                return {"pss_readings_kb": [value, value], "samples": readings}
        if index + 1 < attempts:
            sleep(interval)
    raise ProbeError("pss-unstable", "PSS did not stabilize within the retry limit", samples=readings)

"""Read-only pure-JS QPS fixtures provenance."""

import hashlib
from pathlib import Path

from measurement_runtime import ProbeError


def snapshot_fixtures(apps, out, stacks):
    canonical = Path(__file__).with_name("bench-apps")
    base = []
    if {"capsid", "workerd"} & set(stacks):
        base.append("qps-worker.mjs")
    if {"node", "node-jitless"} & set(stacks):
        base.append("qps-node.mjs")
    for name in base:
        if (apps / name).read_bytes() != (canonical / name).read_bytes():
            raise ProbeError(
                "qps-fixture", "fixture differs from the maintained QPS workload", file=name
            )
    fixtures, inputs = {}, {}
    for name in sorted(set(base)):
        source = apps / name
        if source.is_symlink():
            raise ProbeError("qps-fixture", "fixture symlinks are not supported")
        data = source.read_bytes()
        target = out / "fixtures" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        value = hashlib.sha256(data).hexdigest()
        fixtures[name] = {"file": target.relative_to(out).as_posix(), "sha256": value}
        inputs[str(target)] = value
    return fixtures, inputs

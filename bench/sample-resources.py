#!/usr/bin/env python3
"""Per-process resource sampler for the three-stack benchmark.

Scans the selected process trees every INTERVAL seconds and records, for every
process whose cmdline matches one of the bench roles (capsid host, capsid
worker, gunicorn, uvicorn, php-fpm, nginx), one JSON line:

  {"ts": unix_epoch, "role": role, "pid": pid, "cmd": short_cmd,
   "cpu_pct": window-average %CPU, "pss_kb": PSS from smaps_rollup,
   "rss_kb": RSS from statm}

Container processes (php-fpm/nginx inside the bench container) are ordinary
host PIDs in /proc, so the same sampler covers them without docker exec.

Roles:
  capsid-host    -> capsid-host with --mode static-pool
  capsid-worker  -> capsid-worker (child of capsid-host)
  gunicorn-master -> gunicorn master
  gunicorn-worker -> gunicorn worker
  php-fpm-master -> php-fpm: master process
  php-fpm-child  -> php-fpm pool child
  nginx          -> nginx master/worker

Usage: python3 bench/sample-resources.py --out OUT.jsonl [--interval 5]
       [--root ROLE=process_tree_root] ...  (optional roots; without roots
       the whole table is scanned and matched by cmdline)
"""
import argparse
import json
import os
import re
import time

CLK_TCK = os.sysconf("SC_CLK_TCK")


def read_first_line(path, default=""):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.readline().rstrip("\n")
    except OSError:
        return default


def read_cmdline(pid):
    return read_first_line(f"/proc/{pid}/cmdline").replace("\0", " ").strip()


def stat_fields(pid):
    """Returns (utime, stime) in clock ticks, or None."""
    stat = read_first_line(f"/proc/{pid}/stat")
    if not stat:
        return None
    # comm may contain spaces/parens; find the last ')' then the fields start.
    rparen = stat.rfind(")")
    if rparen < 0:
        return None
    fields = stat[rparen + 1:].split()
    # field 3 (state) is index 0 after ')'; utime is field 14 -> idx 11,
    # stime is field 15 -> idx 12.
    if len(fields) < 13:
        return None
    try:
        return (int(fields[11]), int(fields[12]))
    except ValueError:
        return None


def parent_pid(pid):
    stat = read_first_line(f"/proc/{pid}/stat")
    if not stat:
        return None
    rparen = stat.rfind(")")
    if rparen < 0:
        return None
    fields = stat[rparen + 1:].split()
    if len(fields) < 2:
        return None
    try:
        return int(fields[1])
    except ValueError:
        return None


def smaps_pss_kb(pid):
    total = 0
    try:
        with open(f"/proc/{pid}/smaps_rollup", "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("Pss:"):
                    total = int(line.split()[1])
                    break
    except OSError:
        return None
    return total


def statm_rss_kb(pid):
    line = read_first_line(f"/proc/{pid}/statm")
    if not line:
        return None
    try:
        pages = int(line.split()[1])
    except (ValueError, IndexError):
        return None
    return pages * 4  # 4 KiB pages


ROLE_PATTERNS = [
    (r"capsid-host.*--mode static-pool", "capsid-host"),
    (r"capsid-worker", "capsid-worker"),
    (r"gunicorn.*master", "gunicorn-master"),
    (r"gunicorn.*worker", "gunicorn-worker"),
    (r"uvicorn.*--workers", "uvicorn-master"),
    (r"multiprocessing\.spawn.*--multiprocessing-fork", "uvicorn-worker"),
    (r"php-fpm: master", "php-fpm-master"),
    (r"php-fpm: pool", "php-fpm-child"),
    (r"^nginx:", "nginx"),
    (r"python.*flask_app|flask_app", "python-worker"),
]


def classify(cmd):
    for pattern, role in ROLE_PATTERNS:
        if re.search(pattern, cmd):
            return role
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--pid", action="append", type=int, default=[],
                        help="legacy process-tree roots (role autodetected)")
    parser.add_argument("--root", action="append", default=[],
                        help="labeled process-tree root, for example capsid=123")
    args = parser.parse_args()

    labeled_roots = {}
    for value in args.root:
        label, separator, raw_pid = value.partition("=")
        allowed = {"capsid", "gunicorn", "uvicorn", "container"}
        if not separator or label not in allowed:
            parser.error(f"invalid --root {value!r}")
        try:
            labeled_roots[int(raw_pid)] = label
        except ValueError:
            parser.error(f"invalid --root pid in {value!r}")

    seen = {}  # pid -> (role, utime, stime)

    def tick():
        now = time.time()
        all_pids = {int(entry) for entry in os.listdir("/proc")
                    if entry.isdigit()}
        roots = set(args.pid) | set(labeled_roots)
        owners = {pid: labeled_roots.get(pid) for pid in roots & all_pids}
        if roots:
            pids = roots & all_pids
            changed = True
            while changed:
                changed = False
                for pid in all_pids - pids:
                    parent = parent_pid(pid)
                    if parent in pids:
                        pids.add(pid)
                        owners[pid] = owners.get(parent)
                        changed = True
        else:
            pids = all_pids
        rows = []
        for pid in sorted(pids):
            cmd = read_cmdline(pid)
            if not cmd:
                continue
            owner = owners.get(pid)
            if owner == "capsid":
                role = "capsid-host" if pid in labeled_roots else classify(cmd)
            elif owner == "gunicorn":
                role = ("gunicorn-master" if pid in labeled_roots
                        else "gunicorn-worker")
            elif owner == "uvicorn":
                if pid in labeled_roots:
                    role = "uvicorn-master"
                elif ("multiprocessing.spawn" in cmd and
                      "--multiprocessing-fork" in cmd):
                    role = "uvicorn-worker"
                else:
                    role = None
            else:
                role = classify(cmd)
            if role is None:
                continue
            ticks = stat_fields(pid)
            pss = smaps_pss_kb(pid)
            rss = statm_rss_kb(pid)
            prev = seen.get(pid)
            cpu_pct = 0.0
            if prev and ticks and prev[1] is not None and prev[2] is not None:
                delta = (ticks[0] - prev[1]) + (ticks[1] - prev[2])
                cpu_pct = max(0.0, delta / CLK_TCK / args.interval * 100.0)
            seen[pid] = (role, ticks[0] if ticks else None,
                         ticks[1] if ticks else None)
            rows.append({"ts": round(now, 1), "role": role, "pid": pid,
                         "cmd": cmd[:120], "cpu_pct": round(cpu_pct, 1),
                         "pss_kb": pss, "rss_kb": rss})
        if rows:
            with open(args.out, "a", encoding="utf-8") as handle:
                for row in rows:
                    handle.write(json.dumps(row) + "\n")

    while True:
        tick()
        time.sleep(args.interval)


if __name__ == "__main__":
    main()

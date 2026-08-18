#!/usr/bin/env python3
"""Per-process resource sampler for the three-stack benchmark.

Scans the whole /proc table every INTERVAL seconds and records, for every
process whose cmdline matches one of the bench roles (capsid host, capsid
worker, gunicorn, php-fpm, nginx, python worker), one JSON line:

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
       [--pid capsid_host_pid] ...  (optional pin; without pins the whole
       table is scanned and matched by cmdline)
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
                        help="extra pinned pids (role autodetected by cmdline)")
    args = parser.parse_args()

    seen = {}  # pid -> (role, utime, stime)

    def tick():
        now = time.time()
        pids = set(args.pid)
        for entry in os.listdir("/proc"):
            if entry.isdigit():
                pids.add(int(entry))
        rows = []
        for pid in sorted(pids):
            cmd = read_cmdline(pid)
            if not cmd:
                continue
            role = classify(cmd)
            if role is None and pid not in args.pid:
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
            if role is None:
                role = "pinned"
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

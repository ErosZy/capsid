#!/usr/bin/env python3
"""Generate cold-start fixtures: {10k,100k,1M} x {capsid,node,deno}.

The bulk of each fixture is REAL JS source — three alternating function
templates (loop+object literal, class, arrow/map/filter/sort chain) at
real-bundle AST density, not a string literal. The three runtimes load the
same generated body byte-for-byte; only the entry point differs (capsid
export default, node http server, deno serve). Nominal sizes are targets;
the generated body stops just past the target, so the actual size is within
one template (<0.1%) of nominal and is reported.

Usage: python3 bench/gen-cold-start-apps.py <outdir>
"""
import os
import sys

SIZES = {"10k": 10_000, "100k": 100_000, "1m": 1_000_000}


def fn(idx, variant):
    # Three real-code templates, rotated. Template strings are plain
    # concatenation (JS braces must not hit Python f-strings).
    n = "%05d" % idx
    if variant == 0:
        return (
            "function fn" + n + "(seed) {\n"
            "  const items = [];\n"
            "  let total = seed;\n"
            "  for (let i = 0; i < 16; i++) {\n"
            "    const weight = (i * 37 + seed) % 97;\n"
            '    const item = { id: "item-" + i, weight, tags: ["alpha", "beta", i % 3 === 0 ? "gamma" : "delta"] };\n'
            "    total += weight * (i % 4 + 1);\n"
            "    items.push(item);\n"
            "  }\n"
            "  return { seed, total, count: items.length, ratio: total / (seed + 1) };\n"
            "}"
        )
    if variant == 1:
        return (
            "class P" + n + " {\n"
            "  constructor(seed) { this.seed = seed; this.log = []; }\n"
            "  step(n) {\n"
            "    const v = (n * 41 + this.seed) % 89;\n"
            "    this.log.push(v);\n"
            "    return v;\n"
            "  }\n"
            "  summarize() {\n"
            "    return this.log.reduce((a, b) => a + b, 0) / Math.max(1, this.log.length);\n"
            "  }\n"
            "}"
        )
    return (
        "const p" + n + " = (rows) =>\n"
        "  rows\n"
        "    .map((r) => ({ ...r, w: r.v * (r.k % 5 + 1) }))\n"
        "    .filter((r) => r.w > 10)\n"
        "    .sort((a, b) => b.w - a.w)\n"
        "    .slice(0, 8)\n"
        "    .reduce((acc, r) => acc + r.w, 0);"
    )


def body_to(target):
    """Rotate the three templates until the body reaches the target size."""
    parts = []
    size = 0
    idx = 0
    while size < target:
        chunk = fn(idx, idx % 3) + "\n"
        parts.append(chunk)
        size += len(chunk)
        idx += 1
    return "".join(parts), idx


def capsid_entry():
    return (
        "export default {\n"
        "  async fetch(req) {\n"
        '    const q = fn00000(parseInt(req.headers.get("x-seed") || "7", 10));\n'
        "    return new Response(\n"
        "      JSON.stringify({ status: \"ok\", total: q.total, count: q.count }),\n"
        "      { headers: { \"content-type\": \"application/json\" } },\n"
        "    );\n"
        "  },\n"
        "};\n"
    )


def node_entry():
    return (
        "import { createServer } from \"node:http\";\n"
        "const server = createServer((req, res) => {\n"
        "  const q = fn00000(7);\n"
        "  res.setHeader(\"content-type\", \"application/json\");\n"
        "  res.end(JSON.stringify({ status: \"ok\", total: q.total, count: q.count }));\n"
        "});\n"
        "server.listen(18990, \"127.0.0.1\", () => console.log(\"READY\"));\n"
    )


def deno_entry():
    return (
        "const server = Deno.serve({ port: 18991, hostname: \"127.0.0.1\" },\n"
        "  (req) => {\n"
        "    const q = fn00000(7);\n"
        "    return new Response(\n"
        "      JSON.stringify({ status: \"ok\", total: q.total, count: q.count }),\n"
        "      { headers: { \"content-type\": \"application/json\" } },\n"
        "    );\n"
        "  },\n"
        ");\n"
        "console.log(\"READY\");\n"
    )


def write(outdir, name, body):
    path = os.path.join(outdir, name)
    with open(path, "w") as f:
        f.write(body)
    return path


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for label, target in SIZES.items():
        body, count = body_to(target)
        capsid = body + capsid_entry()
        node = body + node_entry()
        deno = body + deno_entry()
        p1 = write(outdir, "app-capsid-%s.mjs" % label, capsid)
        p2 = write(outdir, "app-node-%s.mjs" % label, node)
        p3 = write(outdir, "app-deno-%s.mjs" % label, deno)
        print("%s: %d top-level units, capsid=%d node=%d deno=%d bytes" %
              (label, count, os.path.getsize(p1), os.path.getsize(p2),
               os.path.getsize(p3)))


if __name__ == "__main__":
    main()

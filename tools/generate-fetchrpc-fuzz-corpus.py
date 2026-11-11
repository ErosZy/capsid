#!/usr/bin/env python3
"""Generate or verify named v4 seeds while preserving hand-written corpora."""
from pathlib import Path
import argparse
import hashlib
import json
import re

parser = argparse.ArgumentParser(description=__doc__)
mode = parser.add_mutually_exclusive_group(required=True)
mode.add_argument('--check', action='store_true')
mode.add_argument('--write', action='store_true')
parser.add_argument('--source', type=Path)
parser.add_argument('--output-root', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
relative_source = 'tests/fixtures/fetchrpc-v4/vectors.json'
source = args.source or root / relative_source
destination = args.output_root or root
raw = source.read_bytes()
vectors = json.loads(raw)
if vectors.get('schema_version') != 2 or not isinstance(vectors.get('frames'), list):
    raise SystemExit('expected FetchRPC v4 vectors schema v2')
generated = {}
index = dict(schema='capsid-fetchrpc-v4-fuzz-corpus-v1', source=relative_source,
             sourceSHA256=hashlib.sha256(raw).hexdigest(),
             legacySeedsPreserved=True, seeds={})
seen = set()
for vector in vectors['frames']:
    name = vector['name']
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or name in seen:
        raise SystemExit('unsafe or duplicate golden-vector name')
    seen.add(name)
    wire = bytes.fromhex(vector['hex'])
    if not wire or len(wire) > 65536:
        raise SystemExit('golden frame exceeds the registered fuzz input budget: ' + name)
    for family in ['protocol', 'worker_protocol']:
        relative = 'tests/fuzz/corpus/' + family + '/golden-v4-' + name
        generated[relative] = wire
        index['seeds'][relative] = dict(vector=name, expect=vector['expect'], bytes=len(wire),
                                       sha256=hashlib.sha256(wire).hexdigest())
if not seen:
    raise SystemExit('zero golden frames is not a corpus')
for family in ['protocol', 'worker_protocol']:
    directory = destination / 'tests/fuzz/corpus' / family
    expected = {Path(name).name for name in generated if Path(name).parent.name == family}
    actual = {p.name for p in directory.glob('golden-v4-*')}
    if actual - expected:
        raise SystemExit('unexpected generated seed(s): ' + ', '.join(sorted(actual - expected)))
index_name = 'tests/fuzz/fetchrpc-v4-corpus.json'
generated[index_name] = (json.dumps(index, sort_keys=True, indent=2) + '\n').encode()
for relative, payload in sorted(generated.items()):
    path = destination / relative
    if args.check:
        if not path.is_file() or path.read_bytes() != payload:
            raise SystemExit('stale or missing generated fuzz input: ' + relative)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
print(json.dumps(dict(mode='check' if args.check else 'write', goldenFrames=len(seen),
                      generatedSeeds=len(index['seeds']), sourceSHA256=index['sourceSHA256'])))

#!/usr/bin/env python3
"""Generate or verify the state, WebSocket frame and handshake boundary seeds."""
from pathlib import Path
import argparse
import hashlib
import json

parser = argparse.ArgumentParser(description=__doc__)
mode = parser.add_mutually_exclusive_group(required=True)
mode.add_argument('--check', action='store_true')
mode.add_argument('--write', action='store_true')
parser.add_argument('--output-root', type=Path)
args = parser.parse_args()
root = args.output_root or Path(__file__).resolve().parents[1]
generated = {}

def seed(family, name, value):
    assert value and len(value) <= 65536
    generated['tests/fuzz/corpus/' + family + '/' + name] = value

for name, value in {
    'single-action': b'\x00', 'all-action-selectors': bytes(range(64)),
    'overflow-fields': b'\xff' * 256, 'large-structured-input': bytes(range(256)) * 256,
}.items():
    seed('channel_registry', name, value)
for name, value in {
    'empty-frame': bytes([0x81, 0]),
    'masked-hello': bytes.fromhex('818537fa213d7f9f4d5158'),
    'masked-empty': bytes.fromhex('828000000000'),
    'length126': bytes([0x82, 126, 0, 126]) + bytes(range(126)),
    'length64-header': bytes.fromhex('827f0000000000010000'),
    'close1000': bytes.fromhex('880203e8'),
    'invalid-close-utf8': bytes.fromhex('880303e8ff'),
    'fragmented-control': bytes.fromhex('098000000000'),
    'actual64k-body': bytes(range(256)) * 256,
}.items():
    seed('websocket_frames', name, value)
for n in range(12):
    seed('websocket_handshake', 'request-mutation-' + str(n), bytes([n]))
key = 'dGhlIHNhbXBsZSBub25jZQ=='
head = ('GET /@capsid/fuzz/ws HTTP/1.1\r\nHost: fuzz.test\r\nConnection: Upgrade\r\n'
        'Upgrade: websocket\r\nSec-WebSocket-Key: ' + key + '\r\nSec-WebSocket-Version: 13\r\n\r\n')
for name, value in [
    ('valid-http', head),
    ('duplicate-key', head[:-2] + 'Sec-WebSocket-Key: ' + key + '\r\n\r\n'),
    ('bodyful-http', head[:-2] + 'Content-Length: 1\r\n\r\nx'),
    ('huge-body-header', head[:-2] + 'Content-Length: 16777216\r\n\r\n'),
    ('incomplete-http', 'GET / HTTP/1.1\r\n'),
]:
    seed('websocket_handshake', name, value.encode())
for length in [8000, 8191, 8192, 8193, 65536]:
    prefix = head[:-2] + 'X-Large: '
    value = prefix + 'x' * (length - len(prefix) - 4) + '\r\n\r\n'
    assert len(value) == length
    seed('websocket_handshake', 'header-boundary-' + str(length), value.encode())
seed('websocket_handshake', 'multiple-invalid-header', (head[:-2] + 'X-Large: ' + 'x' * 8500 + '\x01\r\n\r\n').encode())
index = dict(schema='capsid-channel-websocket-fuzz-corpus-v1', maxInputBytes=65536, seeds={
    name: dict(bytes=len(value), sha256=hashlib.sha256(value).hexdigest()) for name, value in sorted(generated.items())})
generated['tests/fuzz/channel-websocket-corpus.json'] = (json.dumps(index, indent=2) + '\n').encode()
if args.write:
    for name, value in generated.items():
        file = root / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(value)
else:
    problems = []
    for name, value in generated.items():
        file = root / name
        if not file.is_file() or file.read_bytes() != value:
            problems.append('missing or stale: ' + name)
    expected = set(generated)
    for family in ['channel_registry', 'websocket_frames', 'websocket_handshake']:
        directory = root / 'tests/fuzz/corpus' / family
        if directory.exists():
            for file in directory.rglob('*'):
                if file.is_file() and file.relative_to(root).as_posix() not in expected:
                    problems.append('unexpected seed: ' + file.relative_to(root).as_posix())
    if problems:
        raise SystemExit('\n'.join(problems))
print(json.dumps(dict(schema=index['schema'], mode='check' if args.check else 'write', seeds=len(index['seeds']))))

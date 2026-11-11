#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';

// Download the source_url in the lock, then audit the exact upstream bytes and
// every packed code point. Builds consume the reviewed packed source offline.
const [input] = process.argv.slice(2);
assert.ok(input, 'usage: verify-gb18030-source.mjs <downloaded-index-gb18030.txt>');
const lock = JSON.parse(fs.readFileSync(new URL('./encoding-data/gb18030.lock.json', import.meta.url)));
const raw = fs.readFileSync(input);
assert.equal(createHash('sha256').update(raw).digest('hex'), lock.source_sha256);
const rows = [...raw.toString('utf8').matchAll(/^\s*(\d+)\s+0x([\da-f]+)/gmi)];
assert.equal(rows.length, lock.pointer_count);
const packed = Buffer.alloc(rows.length * 2);
for (let i = 0; i < rows.length; ++i) {
    assert.equal(Number(rows[i][1]), i);
    packed.writeUInt16LE(parseInt(rows[i][2], 16), i * 2);
}
assert.equal(createHash('sha256').update(packed).digest('hex'), lock.packed_sha256);
assert.deepEqual(packed, fs.readFileSync(new URL('./encoding-data/gb18030.u16le', import.meta.url)));
console.log(`Verified original WHATWG source and all ${rows.length} packed GB18030 entries`);

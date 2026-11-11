#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';

const [ wptRoot, outputPath, check ] = process.argv.slice(2);
if (!wptRoot || !outputPath || (check !== undefined && check !== '--check')) {
    throw new TypeError(
        'usage: generate-multibyte-indexes.mjs <wpt-root> <output> [--check]');
}

const sources = {
    big5: 'encoding/legacy-mb-tchinese/big5/big5_index.js',
    jis0208: 'encoding/legacy-mb-japanese/euc-jp/jis0208_index.js',
    jis0212: 'encoding/legacy-mb-japanese/euc-jp/jis0212_index.js',
    euckr: 'encoding/legacy-mb-korean/euc-kr/euckr_index.js',
};

const parseIndex = relativePath => {
    const source = fs.readFileSync(path.join(wptRoot, relativePath), 'utf8');
    const match = source.match(/var\s+\w+\s*=\s*(\[[\s\S]*\]);?\s*$/);
    if (!match) {
        throw new TypeError(`cannot parse WPT index: ${relativePath}`);
    }
    return JSON.parse(match[1]);
};

const encodeIndex = index => {
    const bytes = [];
    for (const codePoint of index) {
        let value = codePoint === null ? 0 : codePoint + 1;
        do {
            let byte = value & 0x7F;
            value >>>= 7;
            if (value !== 0) {
                byte |= 0x80;
            }
            bytes.push(byte);
        } while (value !== 0);
    }
    return Buffer.from(bytes).toString('base64');
};

const records = Object.entries(sources).map(([ name, source ]) => {
    const index = parseIndex(source);
    return { name, source, length: index.length, data: encodeIndex(index) };
});
const gbLock = JSON.parse(fs.readFileSync(new URL('./encoding-data/gb18030.lock.json', import.meta.url)));
const gbBytes = fs.readFileSync(new URL('./encoding-data/gb18030.u16le', import.meta.url));
assert.equal(gbLock.pointer_count, 23940);
assert.equal(gbBytes.length, gbLock.pointer_count * 2);
assert.equal(createHash('sha256').update(gbBytes).digest('hex'), gbLock.packed_sha256,
    'GB18030 source index differs from its reviewed lock');
const gbIndex = Array.from({length: gbLock.pointer_count}, (_, i) => gbBytes.readUInt16LE(i * 2));
records.unshift({name: 'gb18030', length: gbIndex.length, data: encodeIndex(gbIndex)});

const lines = [
    '// Generated from the locked WHATWG Encoding indexes in WPT and tools/encoding-data.',
    '// Regenerate with tools/generate-multibyte-indexes.mjs.',
    'const encodedIndexes = Object.freeze({',
];
for (const record of records) {
    lines.push(
        `    ${record.name}: Object.freeze({`,
        `        length: ${record.length},`,
        `        data: '${record.data}',`,
        '    }),');
}
lines.push(
    '});',
    '',
    'const decodedIndexes = new Map();',
    '',
    'function decodeIndex(name) {',
    '    let index = decodedIndexes.get(name);',
    '    if (index !== undefined) {',
    '        return index;',
    '    }',
    '    const record = encodedIndexes[name];',
    '    if (record === undefined) {',
    '        throw new TypeError(`unknown multibyte index: ${name}`);',
    '    }',
    '    const binary = atob(record.data);',
    '    index = new Uint32Array(record.length);',
    '    let byteOffset = 0;',
    '    for (let pointer = 0; pointer < record.length; ++pointer) {',
    '        let value = 0;',
    '        let shift = 0;',
    '        let byte;',
    '        do {',
    '            byte = binary.charCodeAt(byteOffset++);',
    '            value |= (byte & 0x7F) << shift;',
    '            shift += 7;',
    '        } while ((byte & 0x80) !== 0);',
    '        index[pointer] = value === 0 ? 0xFFFFFFFF : value - 1;',
    '    }',
    '    decodedIndexes.set(name, index);',
    '    return index;',
    '}',
    '',
    'export function multibyteIndexCodePoint(name, pointer) {',
    '    const index = decodeIndex(name);',
    '    if (pointer < 0 || pointer >= index.length) {',
    '        return null;',
    '    }',
    '    const codePoint = index[pointer];',
    '    return codePoint === 0xFFFFFFFF ? null : codePoint;',
    '}',
    '');

if (check) assert.equal(fs.readFileSync(outputPath, 'utf8'), lines.join('\n'), 'stale multibyte indexes');
else fs.writeFileSync(outputPath, lines.join('\n'));
console.log(`${check ? 'Verified' : 'Generated'} ${records.length} locked multibyte indexes`);

import fs from 'node:fs';
import {gzipSync, constants} from 'node:zlib';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
assert.equal(process.version, 'v24.14.0');
let result = '/* Deterministic independent Node 24.14.0 zlib inputs. Regenerate with\n'
    + ' * tests/helpers/make_http_gzip_models.mjs; product decoding uses miniz. */\n';
const models = [
    ['gzip_fixed', 8197, false, {strategy: constants.Z_FIXED}],
    ['gzip_dynamic', 16389, false, {}], ['gzip_entropy', 16389, true, {}],
];
const rows = [];
for (const [name, length, entropy, options] of models) {
    const bytes = Buffer.from(Array.from({length}, (_, index) => (index * 29 + 17) % 251));
    if (entropy) {
        let value = 0x12345678;
        for (let index = 0; index < length; ++index) {value ^= value << 13; value ^= value >>> 17; value ^= value << 5; bytes[index] = value & 255;}
    }
    const wire = gzipSync(bytes, options); wire[9] = 255;
    assert.equal((wire[10] >> 1) & 3, name === 'gzip_fixed' ? 1 : name === 'gzip_dynamic' ? 2 : 0);
    result += 'static const uint8_t '+name+'[] = {\n';
    for (let offset = 0; offset < wire.length; offset += 24)
        result += '    '+Array.from(wire.subarray(offset, offset + 24), value => '0x'+value.toString(16).padStart(2, '0')).join(', ')+',\n';
    result += '};\n';
    rows.push({name, plainBytes: bytes.length, wireBytes: wire.length, sha256: createHash('sha256').update(wire).digest('hex')});
}
const target = new URL('../fixtures/http-gzip-models.h', import.meta.url);
fs.writeFileSync(target, result);
console.log(JSON.stringify({complete: true, models: rows}));

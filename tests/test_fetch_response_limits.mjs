import assert from 'node:assert/strict';
import {gunzipSync} from 'node:zlib';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {validateResponseLimitTranscript} from '../tools/fetch-response-limit-contract.mjs';

const [driver, worker, variant] = process.argv.slice(2);
assert.ok(driver && worker && (process.argv.length === 4 ||
    (process.argv.length === 5 && variant === '--encoded-headers')), 'actual C ABI paths and optional encoded-header control');
if (variant) {
    const vectors = spawnSync(driver, ['--gzip-source-vectors'], {encoding:'utf8',timeout:5000,maxBuffer:65536});
    assert.ifError(vectors.error); assert.equal(vectors.status,0); assert.equal(vectors.signal,null); assert.equal(vectors.stderr,'');
    process.stdout.write('GZIP_SOURCE_VECTORS '+vectors.stdout.trim().split(/\r?\n/).length+'\n');
    const rows = vectors.stdout.trim().split(/\r?\n/).map(line=>JSON.parse(line));
    assert.equal(rows.length,8);
    assert.equal(new Set(rows.map(row=>`${row.length}/${row.fragmented}`)).size,8);
    for (const row of rows) {
        const wire = Buffer.concat(row.parts.map(bytes=>Buffer.from(bytes)));
        assert.deepEqual(gunzipSync(wire),Buffer.from(Array.from({length:row.length},(_,i)=>i+1)));
        assert.equal(wire.length,23+row.length+(row.length?(row.fragmented?15:5):0));
        if (row.fragmented && row.length) {
            assert.equal(row.parts[0].length,17); assert.equal(row.parts[1].length,8);
            assert.throws(()=>gunzipSync(Buffer.concat(row.parts.slice(0,2).map(bytes=>Buffer.from(bytes)))));
        }
        const corrupt = Buffer.from(wire); corrupt[corrupt.length-8] ^= 1;
        assert.throws(()=>gunzipSync(corrupt));
    }
}
const fixture = fileURLToPath(new URL('./fixtures/fetch-response-limits.mjs', import.meta.url));
const result = spawnSync(driver, [worker, fixture, ...(variant ? [variant] : [])], {encoding: 'utf8', timeout: 45000, maxBuffer: 4 * 1024 * 1024});
// Preserve all actual observations even if the independent model rejects one.
process.stdout.write(result.stdout ?? '');
process.stderr.write(result.stderr ?? '');
assert.ifError(result.error);
assert.equal(result.signal, null, 'natural driver completion');
assert.equal(result.status, 0, 'real configured workers, native source and retirement');
assert.equal(result.stderr, '', 'no worker, IPC or sanitizer diagnostics');
console.log(JSON.stringify(validateResponseLimitTranscript(result.stdout, {encoding: variant ? 'gzip' : null})));

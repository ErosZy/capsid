import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';

// Actual pinned workerd observations. Archive controls clone these only to
// exercise validation; the real gate requires reports from every backend.
export function readReaderIntrinsicSample() {
    const metadata = JSON.parse(fs.readFileSync(new URL('../fixtures/workerd-reader-intrinsic-report.json', import.meta.url)));
    assert.equal(metadata.schema, 'capsid-reference-sample-gzip-v1');
    const packed = fs.readFileSync(new URL('../fixtures/workerd-reader-intrinsic-report.json.gz', import.meta.url));
    assert.equal(createHash('sha256').update(packed).digest('hex'), metadata.sha256);
    const bytes = gunzipSync(packed); assert.equal(bytes.length, metadata.bytes);
    const report = JSON.parse(bytes);
    assert.equal(report.mode, 'reference'); assert.equal(report.cases.length, 40);
    return report;
}

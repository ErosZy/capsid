import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';
export function readStandardReaderSample() {
    const metadata=JSON.parse(fs.readFileSync(new URL('../fixtures/workerd-standard-reader-report.json',import.meta.url)));
    assert.equal(metadata.schema,'capsid-standard-reader-capture-v1');
    const packed=fs.readFileSync(new URL('../fixtures/workerd-standard-reader-report.json.gz',import.meta.url));
    const sha=value=>createHash('sha256').update(value).digest('hex');assert.equal(sha(packed),metadata.gzipSHA256);
    const raw=gunzipSync(packed);assert.equal(raw.length,metadata.rawBytes);assert.equal(sha(raw),metadata.sourceReportSHA256);
    const report=JSON.parse(raw);assert.equal(report.mode,'reference');assert.equal(report.cases.length,230);
    assert.equal(report.referenceLock.source_commit,metadata.sourceCommit);
    assert.equal(report.referenceLock.binary_sha256,metadata.referenceBinarySHA256);return report;
}

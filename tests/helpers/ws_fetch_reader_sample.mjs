import assert from 'node:assert/strict';
import fs from 'node:fs';
import {gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';

export function readNativeReaderSample(){
    const metadata=JSON.parse(fs.readFileSync(new URL('../fixtures/workerd-fetch-reader-report.json',import.meta.url),'utf8'));
    assert.equal(metadata.schema,'capsid-fetch-reader-reference-capture-v1');assert.equal(metadata.platform,'linux');assert.equal(metadata.cases,14);
    const packed=fs.readFileSync(new URL('../fixtures/workerd-fetch-reader-report.json.gz',import.meta.url));
    const sha=value=>createHash('sha256').update(value).digest('hex');assert.equal(sha(packed),metadata.gzipSHA256);
    const raw=gunzipSync(packed);assert.equal(raw.length,metadata.rawBytes);assert.equal(sha(raw),metadata.sourceReportSHA256);
    const report=JSON.parse(raw);assert.equal(report.cases.length,14);assert.equal(report.schema,'capsid-fetch-readers-v1');
    assert.equal(report.referenceLock.source_commit,metadata.sourceCommit);assert.equal(report.referenceLock.binary_sha256,metadata.referenceBinarySHA256);
    assert.equal(report.binaries.host,metadata.referenceBinarySHA256);return report;
}

import assert from 'node:assert/strict';
import fs from 'node:fs';
import {gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';

// This is an actual native Linux workerd capture. Copies used to exercise
// validators do not qualify another platform or a product executable.
export function readFetchProxySample() {
    const metadata=JSON.parse(fs.readFileSync(new URL('../fixtures/workerd-fetch-proxy-report.json',import.meta.url),'utf8'));
    assert.equal(metadata.schema,'capsid-fetch-proxy-reference-capture-v1');
    const packed=fs.readFileSync(new URL('../fixtures/workerd-fetch-proxy-report.json.gz',import.meta.url));
    const digest=data=>createHash('sha256').update(data).digest('hex');
    assert.equal(digest(packed),metadata.gzipSHA256);const raw=gunzipSync(packed);
    assert.equal(raw.length,metadata.rawBytes);assert.equal(digest(raw),metadata.sourceReportSHA256);
    const report=JSON.parse(raw);assert.equal(report.cases.length,metadata.cases);
    assert.equal(report.referenceLock.source_commit,metadata.sourceCommit);
    assert.equal(report.referenceLock.binary_sha256,metadata.referenceBinarySHA256);
    assert.equal(report.binaries.host,metadata.referenceBinarySHA256);return report;
}

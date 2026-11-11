import assert from 'node:assert/strict';
import fs from 'node:fs';
import {gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';

export function readNativeArgumentSample(){
    const metadata=JSON.parse(fs.readFileSync(new URL('../fixtures/workerd-fetch-arguments-report.json',import.meta.url),'utf8'));
    assert.equal(metadata.schema,'capsid-fetch-arguments-reference-capture-v1');assert.equal(metadata.platform,'linux');
    const packed=fs.readFileSync(new URL('../fixtures/workerd-fetch-arguments-report.json.gz',import.meta.url));
    const sha=value=>createHash('sha256').update(value).digest('hex');assert.equal(sha(packed),metadata.gzipSHA256);
    const raw=gunzipSync(packed);assert.equal(raw.length,metadata.rawBytes);assert.equal(sha(raw),metadata.sourceReportSHA256);
    const report=JSON.parse(raw);assert.equal(report.schema,'capsid-fetch-arguments-v1');assert.equal(report.mode,'reference');
    assert.equal(metadata.groups,130);assert.equal(report.cases.length,130);assert.equal(metadata.observations,5460);
    assert.equal(report.cases.reduce((sum,value)=>sum+JSON.parse(value.observed.text).state.rows.length,0),5460);
    assert.equal(report.cases.reduce((sum,value)=>sum+value.upstreamBeforeStop.records.length,0),5460);
    assert.equal(report.referenceLock.source_commit,metadata.sourceCommit);assert.equal(report.referenceLock.binary_sha256,metadata.referenceBinarySHA256);
    assert.equal(report.binaries.host,metadata.referenceBinarySHA256);return report;
}

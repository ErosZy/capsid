import assert from 'node:assert/strict';
import fs from 'node:fs';
import {gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';

// An actual Linux reference capture. Copies made by validator controls are
// synthetic exercises; each real gate still requires every native backend.
export function readFetchBYOBSample(){
    const metadata=JSON.parse(fs.readFileSync(new URL('../fixtures/workerd-fetch-byob-report.json',import.meta.url),'utf8'));
    assert.equal(metadata.schema,'capsid-fetch-byob-reference-capture-v1');assert.equal(metadata.platform,'linux');
    const packed=fs.readFileSync(new URL('../fixtures/workerd-fetch-byob-report.json.gz',import.meta.url));
    const sha=value=>createHash('sha256').update(value).digest('hex');assert.equal(sha(packed),metadata.gzipSHA256);
    const raw=gunzipSync(packed);assert.equal(raw.length,metadata.rawBytes);assert.equal(sha(raw),metadata.sourceReportSHA256);
    const report=JSON.parse(raw);assert.equal(report.cases.length,metadata.cases);assert.equal(metadata.cases,487);
    assert.equal(metadata.nativeCases,330);assert.equal(metadata.standardControls,116);assert.equal(metadata.readerBrandCases,10);
    assert.equal(report.cases.filter(value=>value.kind.startsWith('native-')).length,330);
    assert.equal(report.cases.filter(value=>value.kind.startsWith('standard-')).length,116);
    assert.equal(report.cases.filter(value=>value.kind.startsWith('brand-')).length,10);
    assert.equal(metadata.nativeBodyCases,21);assert.equal(report.cases.filter(value=>value.kind.startsWith('body-')).length,21);
    assert.equal(metadata.validationCases,10);assert.equal(metadata.validationRows,420);
    const validation=report.cases.filter(value=>value.kind.startsWith('validation-'));assert.equal(validation.length,10);
    assert.equal(validation.reduce((sum,value)=>sum+JSON.parse(value.observed.text).state.rows.length,0),420);
    assert.equal(metadata.standardCloseCases,16);assert.equal(metadata.standardCloseRows,528);
    const temporal=report.cases.filter(value=>value.kind.startsWith('standard-close-'));assert.equal(temporal.length,16);
    assert.equal(temporal.reduce((sum,value)=>sum+JSON.parse(value.observed.text).state.rows.length,0),528);
    assert.equal(report.referenceLock.source_commit,metadata.sourceCommit);assert.equal(report.referenceLock.binary_sha256,metadata.referenceBinarySHA256);
    assert.equal(report.binaries.host,metadata.referenceBinarySHA256);return report;
}

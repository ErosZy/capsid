import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {parseArgs,isDeepStrictEqual} from 'node:util';
import {fileURLToPath,pathToFileURL} from 'node:url';

const digest=file=>createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const read=file=>JSON.parse(fs.readFileSync(file,'utf8'));
const rawNames=['last-test','junit','registration','gate','build','wpt-root','context','ctest-exit-code'];

// Re-evaluate the complete archived reference/context evidence. The accepted
// summary alone cannot waive standard failures in an upgrade report.
export function assertReleaseAcceptance(record,ordinary,registration,profile) {
    assert.equal(record.schema,'capsid-workerd-compatible-release-acceptance-v1');
    assert.equal(record.complete,true);
    assert.equal(record.status,'compatible-with-retained-standard-failures');
    assert.equal(record.compatibilityQualified,true);
    assert.equal(record.standardsConformanceClaimed,false);
    assert.equal(record.ordinarySuitePassed,false);
    assert.equal(record.rawCTestExitText,'8\n');
    assert.equal(record.rawStepOutcome,'failure');
    assert.equal(record.privilegedSandboxAndOtherHostedJobsStillMandatory,true);
    assert.deepEqual(record.ordinary,ordinary,'same complete ordinary suite');
    assert.deepEqual(record.rawFullSuite,{selected:ordinary.selected,failed:ordinary.failed,skipped:ordinary.skipped},
        'same untouched actual raw suite');
    assert.equal(profile.fullSuite.selected,registration.tests.length);
}

export async function replayReleaseAcceptance(values) {
    const source=path.resolve(values['source-root']);
    const roles=Object.fromEntries(rawNames.map(name=>[name,path.resolve(values[name])]));
    const acceptance=path.resolve(values.acceptance),record=read(acceptance),before=digest(acceptance);
    const {checkOrdinaryMatrix,checkCompleteCompatibility}=await import(pathToFileURL(path.join(source,'tools/workerd-wpt-acceptance.mjs')));
    const {profileV2File}=await import(pathToFileURL(path.join(source,'tools/workerd-wpt-profile-v2.mjs')));
    const ext=process.platform==='win32'?'.exe':'';
    const protectedFiles=[roles['last-test'],roles.junit,roles.registration,roles.context,roles['ctest-exit-code'],
        path.join(source,profileV2File),path.join(source,'tests/wpt/manifest.json'),
        path.join(roles.build,'capsid-host'+ext),path.join(roles.build,'capsid-worker'+ext),
        path.join(source,'tools/workerd-wpt-acceptance.mjs'),path.join(source,'tools/workerd-wpt-profile-v2.mjs')];
    assert.ok(record.inputSHA256&&typeof record.inputSHA256==='object');
    assert.deepEqual(Object.keys(record.inputSHA256).sort(),protectedFiles.map(file=>path.resolve(file)).sort(),
        'all acceptance input roles must bind the supplied source, raw suite and binaries');
    for(const file of protectedFiles)assert.equal(digest(file),record.inputSHA256[path.resolve(file)],'unchanged acceptance input: '+file);
    const originalProfile=path.resolve(record.profileAudit);
    assert.equal(digest(originalProfile),record.profileAuditSHA256,'accepted profile artifact bytes');
    const profile=read(originalProfile),registration=read(roles.registration);
    const python=values.python??process.env.CAPSID_WS_INTEROP_PYTHON??'python3';
    const parser='import json,sys,xml.etree.ElementTree as E\nr=E.parse(sys.argv[1]).getroot()\nprint(json.dumps([{"name":c.get("name"),"status":"failed" if c.find("failure") is not None or c.find("error") is not None else "skipped" if c.find("skipped") is not None else "passed"} for c in r.iter("testcase")]))';
    const cases=JSON.parse(execFileSync(python,['-I','-c',parser,roles.junit],{encoding:'utf8',timeout:10000}));
    const ordinary=checkOrdinaryMatrix({exitText:fs.readFileSync(roles['ctest-exit-code'],'utf8'),
        outcome:record.rawStepOutcome,registration,cases});
    assertReleaseAcceptance(record,ordinary,registration,profile);
    checkCompleteCompatibility(profile,ordinary,registration);
    const parent=fs.realpathSync(os.tmpdir());
    const temporary=fs.mkdtempSync(path.join(parent,'capsid-release-replay-'));
    let replayedDigest;
    try {
        const output=path.join(temporary,'profile.json');
        const args=[path.join(source,'tools/workerd-wpt-profile-v2.mjs'),'audit'];
        for(const name of rawNames.filter(n=>n!=='ctest-exit-code'))args.push('--'+name,roles[name]);
        args.push('--output',output,'--python',python);
        execFileSync(process.execPath,args,{encoding:'utf8',timeout:600000,maxBuffer:64*1024*1024});
        const replayed=read(output);checkCompleteCompatibility(replayed,ordinary,registration);
        assert.ok(isDeepStrictEqual(replayed,profile),'fresh complete profile replay must reproduce the accepted raw evidence');
        replayedDigest=digest(output);
    } finally {
        assert.equal(path.dirname(fs.realpathSync(temporary)),parent);
        assert.ok(path.basename(temporary).startsWith('capsid-release-replay-'));
        fs.rmSync(temporary,{recursive:true,force:true});
    }
    assert.equal(digest(acceptance),before,'accepted artifact must remain unchanged');
    assert.equal(digest(originalProfile),record.profileAuditSHA256,'original profile artifact must remain unchanged');
    for(const file of protectedFiles)assert.equal(digest(file),record.inputSHA256[path.resolve(file)],'unchanged replay input: '+file);
    return {schema:'capsid-workerd-release-replay-v1',complete:true,compatibilityQualified:true,
        standardsConformanceClaimed:false,ordinarySuitePassed:false,
        profile:'CAPSID-WORKERD-v1.20261010.1-default-v2',ordinary,registration,
        acceptance:{path:acceptance,sha256:before},profileAudit:{path:originalProfile,sha256:record.profileAuditSHA256},
        replayedProfileSHA256:replayedDigest,sourceRoot:source,build:roles.build,junit:roles.junit,
        junitSHA256:digest(roles.junit),inputSHA256:record.inputSHA256,
        validator:{path:fileURLToPath(import.meta.url),sha256:digest(fileURLToPath(import.meta.url))},
        privilegedSandboxAndOtherHostedJobsStillMandatory:true};
}

async function main() {
    const names=['source-root','acceptance',...rawNames,'python'];
    const {values}=parseArgs({args:process.argv.slice(2),options:Object.fromEntries(names.map(name=>[name,{type:'string'}]))});
    for(const name of names.filter(name=>name!=='python'))assert.ok(values[name],'required --'+name);
    console.log(JSON.stringify(await replayReleaseAcceptance(values)));
}
if(process.argv[1]&&fileURLToPath(import.meta.url)===path.resolve(process.argv[1]))await main();

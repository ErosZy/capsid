import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {parseArgs} from 'node:util';
import {fileURLToPath} from 'node:url';
import {profileV2Paths,profileV2File} from './workerd-wpt-profile-v2.mjs';
import {assertAuditOutput} from './workerd-wpt-profile.mjs';
import {root,fileDigest} from './workerd-reference.mjs';
import {requiredTestsFor} from './run-workerd-ws-gate.mjs';

export const ordinaryWorkerdFailures=Object.freeze(profileV2Paths.map(file=>'worker_wpt_'+file.replace(/[^A-Za-z0-9]/g,'_')).sort());

// This preflight checks the untouched full-suite result before expensive
// reference validation. It cannot qualify compatibility by itself.
export function checkOrdinaryMatrix({exitText,outcome,registration,cases}) {
    assert.equal(exitText,'8\n','the raw CTest exit must be exactly the recorded test-failure code');
    assert.equal(outcome,'failure','the raw workflow step must retain its failure outcome');
    assert.ok(Array.isArray(registration.tests)&&registration.tests.length>0,'complete nonempty registration');
    const tests=new Map(registration.tests.map(test=>[test.name,test]));
    assert.equal(tests.size,registration.tests.length,'unique registered names');
    assert.ok(Array.isArray(cases),'parsed complete JUnit cases');
    assert.equal(new Set(cases.map(row=>row.name)).size,cases.length,'unique actual JUnit names');
    assert.deepEqual(cases.map(row=>row.name).sort(),[...tests.keys()].sort(),'every registered test actually reported');
    for(const row of cases) {
        assert.deepEqual(Object.keys(row).sort(),['name','status']);
        assert.ok(['passed','failed','skipped'].includes(row.status),'known JUnit status');
        const properties=Object.fromEntries((tests.get(row.name).properties??[]).map(p=>[p.name,p.value]));
        assert.ok(!properties.DISABLED,'disabled tests cannot qualify release acceptance');
        assert.ok(!properties.WILL_FAIL,'inverted failures cannot qualify release acceptance');
    }
    const failed=cases.filter(row=>row.status==='failed').map(row=>row.name).sort();
    assert.deepEqual(failed,ordinaryWorkerdFailures,'only the five exact raw WPT files may fail');
    const skipped=cases.filter(row=>row.status==='skipped').map(row=>row.name).sort();
    for(const name of skipped) {
        const properties=Object.fromEntries(tests.get(name).properties.map(p=>[p.name,p.value]));
        assert.ok(Array.isArray(properties.LABELS)&&properties.LABELS.includes('sandbox'),
            'ordinary skips require the later mandatory privileged sandbox matrix: '+name);
        assert.equal(properties.SKIP_RETURN_CODE,77,'only declared sandbox prerequisite skips may remain');
        assert.equal(properties.SKIP_REGULAR_EXPRESSION,undefined,'regex skips cannot qualify release acceptance');
    }
    return {selected:cases.length,rawCTestExitCode:8,rawStepOutcome:'failure',failed,skipped,
        ordinarySuitePassed:false,requiresPrivilegedSandboxCoverage:skipped.length>0};
}

export function checkCompleteCompatibility(audit,ordinary,registration) {
    assert.equal(audit.schema,'capsid-workerd-wpt-profile-audit-v2');
    assert.equal(audit.status,'profile-matched-with-standard-failures');
    assert.equal(audit.complete,true,'the actual full profile audit completed');
    assert.equal(audit.profile,'CAPSID-WORKERD-v1.20261010.1-default-v2');
    assert.equal(audit.files,5);assert.equal(audit.totalSubtests,79);
    assert.equal(audit.standardConflictSubtests,23);assert.equal(audit.standardTimeouts,4);
    assert.equal(audit.standardsConformanceClaimed,false);assert.equal(audit.standardCTestFilesPassed,false);
    assert.deepEqual(audit.witnessModes,['reference','single-worker','static-pool','managed'],'all Linux release modes');
    assert.equal(audit.fullSuite.selected,registration.tests.length);
    assert.equal(audit.fullSuite.selected,ordinary.selected);
    assert.equal(audit.fullSuite.ordinarySuitePassed,false);
    assert.deepEqual([...audit.fullSuite.skipped].sort(),ordinary.skipped);
    const lock=JSON.parse(fs.readFileSync(path.join(root,'tests/fixtures/workerd-ws/reference-lock.json'),'utf8'));
    assert.deepEqual([...audit.gate.tests].sort(),requiredTestsFor(lock).sort(),'the exact complete mandatory Linux reference gate');
    assert.ok(audit.inputSHA256&&Object.keys(audit.inputSHA256).length>=8,'full profile input identities');
    return {compatibilityQualified:true,standardsConformanceClaimed:false,ordinarySuitePassed:false};
}

function freshOutput(file) {
    assert.equal(fs.existsSync(file),false,'fresh compatibility acceptance output required');
    let ancestor=path.dirname(path.resolve(file)),suffix=[path.basename(file)];
    while(!fs.existsSync(ancestor)){suffix.unshift(path.basename(ancestor));ancestor=path.dirname(ancestor);}
    return path.join(fs.realpathSync(ancestor),...suffix);
}

function main() {
    assert.equal(process.argv[2],'audit','usage: workerd-wpt-acceptance.mjs audit [full profile inputs]');
    const forwarded=['last-test','junit','registration','gate','build','wpt-root','context','output','python'];
    const added=['ctest-exit-code','ordinary-outcome','acceptance-output'];
    const {values}=parseArgs({args:process.argv.slice(3),options:Object.fromEntries([...forwarded,...added].map(n=>[n,{type:'string'}]))});
    for(const name of [...forwarded.filter(n=>n!=='python'),...added])assert.ok(values[name],'required --'+name);
    const acceptance=freshOutput(values['acceptance-output']),profileOutput=freshOutput(values.output);
    const protectedFiles=[values['last-test'],values.junit,values.registration,values.context,values['ctest-exit-code'],
        path.join(root,profileV2File),path.join(root,'tests/wpt/manifest.json'),
        path.join(values.build,'capsid-host'),path.join(values.build,'capsid-worker'),fileURLToPath(import.meta.url),
        path.join(root,'tools/workerd-wpt-profile-v2.mjs')];
    assertAuditOutput(acceptance,[...protectedFiles,profileOutput],values.gate,values.build);
    assertAuditOutput(profileOutput,[...protectedFiles,acceptance],values.gate,values.build);
    fs.mkdirSync(path.dirname(acceptance),{recursive:true});
    const record={schema:'capsid-workerd-compatible-release-acceptance-v1',complete:false,status:'running',
        standardsConformanceClaimed:false,ordinarySuitePassed:false};
    const save=()=>fs.writeFileSync(acceptance,JSON.stringify(record,null,2)+'\n');save();
    try {
        const before=Object.fromEntries(protectedFiles.map(file=>[path.resolve(file),fileDigest(file)]));
        const registration=JSON.parse(fs.readFileSync(values.registration,'utf8'));
        const python=values.python??process.env.CAPSID_WS_INTEROP_PYTHON??'python3';
        const code='import json,sys,xml.etree.ElementTree as E\nr=E.parse(sys.argv[1]).getroot()\nprint(json.dumps([{"name":c.get("name"),"status":"failed" if c.find("failure") is not None or c.find("error") is not None else "skipped" if c.find("skipped") is not None else "passed"} for c in r.iter("testcase")]))';
        const cases=JSON.parse(execFileSync(python,['-I','-c',code,values.junit],{encoding:'utf8',timeout:10000}));
        const exitText=fs.readFileSync(values['ctest-exit-code'],'utf8');
        Object.assign(record,{inputSHA256:before,rawCTestExitText:exitText,rawStepOutcome:values['ordinary-outcome'],
            rawFullSuite:{selected:cases.length,failed:cases.filter(row=>row.status==='failed').map(row=>row.name).sort(),
                skipped:cases.filter(row=>row.status==='skipped').map(row=>row.name).sort()}});save();
        const ordinary=checkOrdinaryMatrix({exitText,outcome:values['ordinary-outcome'],registration,cases});
        const command=[path.join(root,'tools/workerd-wpt-profile-v2.mjs'),'audit'];
        for(const name of forwarded)if(values[name])command.push('--'+name,name==='output'?profileOutput:values[name]);
        execFileSync(process.execPath,command,{stdio:'inherit',timeout:600000});
        const audit=JSON.parse(fs.readFileSync(profileOutput,'utf8'));
        const qualified=checkCompleteCompatibility(audit,ordinary,registration);
        for(const file of [values['last-test'],values.junit,values.registration,values.context,
            path.join(values.build,'capsid-host'),path.join(values.build,'capsid-worker'),
            path.join(root,profileV2File),path.join(root,'tests/wpt/manifest.json')])
            assert.equal(audit.inputSHA256[path.resolve(file)],before[path.resolve(file)],'same full profile and raw acceptance inputs');
        for(const [file,hash] of Object.entries(before))assert.equal(fileDigest(file),hash,'unchanged acceptance input: '+file);
        Object.assign(record,qualified,{complete:true,status:'compatible-with-retained-standard-failures',ordinary,
            profileAudit:profileOutput,profileAuditSHA256:fileDigest(profileOutput),inputSHA256:before,
            privilegedSandboxAndOtherHostedJobsStillMandatory:true});save();
        console.log(JSON.stringify({status:record.status,rawCTestExitCode:8,rawStepOutcome:'failure',standardFailedFiles:5,ordinarySuitePassed:false}));
    } catch(error) {Object.assign(record,{complete:false,status:'failed',error:String(error.stack??error)});save();throw error;}
}
if(process.argv[1]&&fileURLToPath(import.meta.url)===path.resolve(process.argv[1]))main();

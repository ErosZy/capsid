import assert from 'node:assert/strict';
import { test } from 'node:test';
import * as referenceTools from '../tools/workerd-reference.mjs';
import {abruptOperations,assertOutboundDisconnect,assertOutboundHttpOwnership} from '../tools/workerd-outbound-contract.mjs';

// Exercise the gate with real archived report shapes, then introduce one
// defect at a time. Only the 140 repeated handshake failures are compressed;
// ownership budget repeats and the two ordinary echo invocations stay intact.
import fs from 'node:fs';
const samples = () => JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-outbound-reports.json', import.meta.url)));
function matrix() {
    const { reference, candidates } = samples();
    for (const report of [reference, ...candidates]) {
        const stress = report.cases.find(c => c.name === 'ws://localhost/badkey:echo');
        report.cases.push(...Array.from({length: 140}, () => structuredClone(stress)));
        report.caseInvocations = report.cases.length;
        report.scenarioTypes = new Set(report.cases.map(c => c.name)).size;
    }
    return { reference, candidates, expected: { lock: referenceTools.readLock(new URL('./fixtures/workerd-ws/reference-lock.json', import.meta.url)),
        runner: reference.sourceSHA256, host: candidates[0].hostSHA256, worker: candidates[0].workerSHA256 } };
}

function rawCleanCloseErrorCount(report) {
    let count=0;
    const visit=value=>{
        if(!value||typeof value!=='object')return;
        if(Array.isArray(value)){for(const item of value)visit(item);return;}
        if(Array.isArray(value.events)&&value.events.at(-1)?.type==='close'&&value.events.at(-1).clean===true)
            count+=value.events.filter(event=>event.type==='error').length;
        for(const [key,item] of Object.entries(value))if(key!=='events')visit(item);
    };
    for(const item of report.cases)visit(item.result);
    return count;
}

test('all outbound cases and documented field differences compare', () => {
    const m = matrix();
    assert.equal(referenceTools.compareOutboundReports(m.reference, m.candidates, m.expected).complete, true);
});

test('pinned reference close race is explicit, bounded, and preserves the raw trace', () => {
    const m = matrix();
    const result = m.reference.cases.find(c=>c.name === 'accepted-after-HTTP').result;
    const error = m.reference.cases.find(c=>c.name === 'ws://localhost/echo:cancel').result.events[1];
    const existing=result.events.filter(event=>event.type==='error').length;
    assert.ok(existing===0||existing===1,'raw sample contains at most one reference close-race event');
    if(existing===0)result.events.splice(-1,0,structuredClone(error));
    const before = structuredClone(result);
    assert.equal(referenceTools.compareOutboundReports(m.reference,m.candidates,m.expected).referenceCloseRaceEvents,
        rawCleanCloseErrorCount(m.reference));
    assert.deepEqual(result,before);
    for (const mutate of [
        value => { value.events.at(-2).errorMessage = 'unknown'; },
        value => { value.events.at(-2).state = 1; },
        value => { value.events.at(-1).clean = false; },
        value => { value.events.splice(-1,0,structuredClone(error)); },
        value => { [value.events[2],value.events[3]] = [value.events[3],value.events[2]]; },
    ]) {
        const changed = structuredClone(before); mutate(changed);
        assert.throws(()=>referenceTools.assertOutboundSuccess(changed,['hello'],'chat','workerd'));
    }
    assert.throws(()=>referenceTools.assertOutboundSuccess(before,['hello'],'chat','single-worker'),
        'reference-only behavior cannot hide a Capsid regression');
});

for (const [name, mutate] of [
    ['missing reference', m => { m.reference = undefined; }],
    ['missing mode', m => { m.candidates.pop(); }],
    ['duplicate mode', m => { m.candidates[1].mode = m.candidates[0].mode; }],
    ['incomplete report', m => { m.reference.complete = false; }],
    ['missing case on every backend', m => {
        for (const report of [m.reference, ...m.candidates]) {
            report.cases = report.cases.filter(c => c.name !== 'out-of-order-dials');
            report.caseInvocations = report.cases.length; report.scenarioTypes--;
        }
    }],
    ['dropped stress invocation', m => { m.candidates[0].cases.pop(); m.candidates[0].caseInvocations--; }],
    ['zero assertions', m => { m.candidates[0].assertions = 0; }],
    ['runner changed', m => { m.candidates[0].sourceSHA256 = 'wrong'; }],
    ['binary changed', m => { m.candidates[0].workerSHA256 = 'wrong'; }],
    ['client changed', m => { m.reference.client.node = 'v0'; }],
    ['forced shutdown', m => { m.candidates[0].shutdowns[0].forced = true; }],
    ['wrong data', m => { m.candidates[0].cases.find(c => c.name === 'ws://localhost/echo:echo').result.events[2].data = 'wrong'; }],
    ['string error payload', m => { m.candidates[0].cases.find(c => c.name === 'ws://localhost/badkey:echo').result.events[1].errorType = 'string'; }],
    ['lost HTTP TypeError', m => { m.candidates[0].cases.find(c => c.name === 'ws://localhost/not101:echo').result.events[1].errorName = 'Error'; }],
    ['unknown diagnostic', m => { m.candidates[0].cases.find(c => c.name === 'ws://localhost/badkey:echo').result.events[1].errorMessage = 'anything'; }],
    ['unexpected abrupt state', m => { m.candidates[0].cases.find(c => c.name === 'ws://localhost/abrupt:echo').result.events.at(-1).state = 3; }],
    ['wrong protocol inside exception', m => { m.reference.cases.find(c => c.name === 'ws://localhost/badprotocol:echo').result.events[1].protocol = 'wrong'; }],
    ['pending constructor completes after HTTP', m => {
        m.candidates[0].cases.find(c => c.name === 'pending-after-HTTP').result.push({events:[]});
    }],
    ['missing ownership budget repeat on every backend', m => {
        for (const report of [m.reference,...m.candidates]) {
            report.cases.splice(report.cases.findIndex(c=>c.name === 'owned-pending-stall'),1);
            report.caseInvocations--;
        }
    }],
    ['missing owned host shutdown on every backend', m => {
        for (const report of [m.reference,...m.candidates]) report.shutdowns = report.shutdowns.filter(s=>s.label !== 'owned');
    }],
]) test(`outbound rejects ${name}`, () => {
    const m = matrix(); mutate(m);
    assert.throws(() => referenceTools.compareOutboundReports(m.reference, m.candidates, m.expected));
});

function disconnectSample(operation) {
    return samples().reference.cases.find(c => c.name === `ws://localhost/abrupt:abrupt-${operation}`).result;
}

test('disconnect lifecycle uses all six actual reference observations', () => {
    assert.equal(abruptOperations.length, 6);
    for (const operation of abruptOperations) {
        assertOutboundDisconnect(disconnectSample(operation), operation);
    }
});

for (const [name, operation, mutate] of [
    ['closed during callback', 'observe', r => { r.events.at(-1).state = 3; }],
    ['generic close reason', 'observe', r => { r.events.at(-1).reason = 'WebSocket disconnected'; }],
    ['missing lifecycle observations', 'observe', r => { delete r.lifecycle; }],
    ['no close callback', 'observe', r => { r.lifecycle.closes = 0; }],
    ['duplicate close callback', 'observe', r => { r.lifecycle.closes = 2; }],
    ['duplicate close event', 'observe', r => { r.events.push(structuredClone(r.events.at(-1))); }],
    ['transition before callback returns', 'observe', r => { r.lifecycle.afterCallbackMethods = 2; }],
    ['no transition at microtask', 'observe', r => { r.lifecycle.microtask = 1; }],
    ['closed at timer', 'observe', r => { r.lifecycle.timer = 3; }],
    ['post-timer transition', 'observe', r => { r.lifecycle.afterTimerMethods = 3; }],
    ['send throws inside callback', 'send-in', r => { r.lifecycle.call = {error:'TypeError',state:1}; }],
    ['close changes state inside callback', 'close-in', r => { r.lifecycle.call.state = 2; }],
    ['send throws after callback', 'send-after', r => { r.lifecycle.call = {error:'TypeError',state:2}; }],
    ['close changes state after callback', 'close-after', r => { r.lifecycle.call.state = 3; }],
    ['missing post-EOF method', 'both-after', r => { r.lifecycle.calls.pop(); }],
    ['wrong return from ignored send', 'both-after', r => { r.lifecycle.calls[0].result = 'number'; }],
]) test(`disconnect lifecycle rejects ${name}`, () => {
    const result = disconnectSample(operation);
    mutate(result);
    assert.throws(() => assertOutboundDisconnect(result, operation));
});

function ownershipSample(name, mode = 'workerd') {
    const fixture = samples();
    const report = mode === 'workerd' ? fixture.reference : fixture.candidates.find(r=>r.mode === mode);
    return report.cases.find(c=>c.name === name).result;
}

function assertOwnership(name, result, mode = 'workerd') {
    return assertOutboundHttpOwnership(result,
        name === 'owned-accepted-after-HTTP' || name === 'owned-streamed-before-HTTP-end', mode,
        assert, name === 'owned-streamed-before-HTTP-end');
}

test('HTTP ownership requires the same reference diagnostic in every mode',()=>{
    const result=ownershipSample('owned-pending-success');
    for(const mode of ['workerd','single-worker','static-pool','managed'])
        assertOwnership('owned-pending-success',structuredClone(result),mode);
});

test('HTTP ownership rejects the shortened cross-request diagnostic in every mode',()=>{
    const result=ownershipSample('owned-pending-success');
    result.state.readyState.message='Cannot perform I/O on behalf of a different request';
    for(const mode of ['workerd','single-worker','static-pool','managed'])
        assert.throws(()=>assertOwnership('owned-pending-success',structuredClone(result),mode));
});

test('HTTP ownership compares all real pending, accepted and unfinished-body observations without changing reports', () => {
    const fixture = samples();
    for (const report of [fixture.reference,...fixture.candidates]) {
        const cases = report.cases.filter(c=>c.name.startsWith('owned-'));
        assert.equal(cases.length,7);
        for (const {name,result} of cases) {
            const before = structuredClone(result);
            const normalized = assertOwnership(name,result,report.mode);
            assert.deepEqual(result,before);
            assert.deepEqual(normalized.state.readyState,
                fixture.reference.cases.find(c=>c.name===name).result.state.readyState);
        }
    }
});

test('HTTP ownership accepts only the bounded reference close race and counts its nested raw event', () => {
    const m = matrix();
    const result = m.reference.cases.find(c=>c.name === 'owned-accepted-after-HTTP').result;
    const error = m.reference.cases.find(c=>c.name === 'ws://localhost/echo:cancel').result.events[1];
    const existing=result.state.events.filter(event=>event.type==='error').length;
    assert.ok(existing===0||existing===1,'raw sample contains at most one nested reference close-race event');
    if(existing===0)result.state.events.splice(-1,0,structuredClone(error));
    const before = structuredClone(result);
    assert.equal(referenceTools.compareOutboundReports(m.reference,m.candidates,m.expected).referenceCloseRaceEvents,
        rawCleanCloseErrorCount(m.reference));
    assert.deepEqual(result,before);
    result.state.events.splice(-1,0,structuredClone(error));
    assert.throws(()=>assertOwnership('owned-accepted-after-HTTP',result));
});

for (const [name, scenario, mutate] of [
    ['missing construct event','owned-pending-success',r=>{ r.state.events=[]; }],
    ['wrong constructor state','owned-pending-success',r=>{ r.state.events[0].state=1; }],
    ['early send permitted','owned-pending-success',r=>{ r.state.sendRejected=false; }],
    ['early accept permitted','owned-pending-success',r=>{ r.state.acceptRejected=false; }],
    ['late open callback','owned-pending-success',r=>{ r.state.events.push({type:'open',state:1,protocol:'chat'}); }],
    ['late error callback','owned-pending-reject',r=>{ r.state.events.push({type:'error',state:3}); }],
    ['late close callback','owned-pending-stall',r=>{ r.state.events.push({type:'close',state:3,code:1006}); }],
    ['TCP peer remains open','owned-pending-stall',r=>{ r.peer.closed=false; }],
    ['pending transport sends data','owned-pending-success',r=>{ r.peer.messages.push('reply:after HTTP'); }],
    ['pending transport sends Close','owned-pending-success',r=>{ r.peer.closes.push(1000); }],
    ['pending constructor already open at return','owned-pending-success',r=>{ r.state.returnedState=1; }],
    ['wrong response status','owned-pending-success',r=>{ r.response.status=200; }],
    ['incomplete response body','owned-pending-success',r=>{ r.response.text=''; }],
    ['numeric cross-request readyState','owned-pending-success',r=>{ r.state.readyState=0; }],
    ['cross-request error class changed','owned-pending-success',r=>{ r.state.readyState.error='TypeError'; }],
    ['cross-request diagnostic changed','owned-pending-success',r=>{ r.state.readyState.message='unrelated'; }],
    ['unverified diagnostic suffix','owned-pending-success',r=>{ r.state.readyState.message += '. unknown'; }],
    ['extra ignored error field','owned-pending-success',r=>{ r.state.readyState.ignored=true; }],
    ['missing readyState observation','owned-pending-success',r=>{ delete r.state.readyState; }],
    ['pending negotiated protocol','owned-pending-success',r=>{ r.state.protocol='chat'; }],
    ['pending negotiated extensions','owned-pending-success',r=>{ r.state.extensions=null; }],
    ['accepted constructor not open at return','owned-accepted-after-HTTP',r=>{ r.state.returnedState=0; }],
    ['accepted socket misses post-HTTP message','owned-accepted-after-HTTP',r=>{ r.state.events.splice(2,1); }],
    ['accepted socket corrupts post-HTTP message','owned-accepted-after-HTTP',r=>{ r.state.events[2].data='wrong'; }],
    ['accepted socket closes uncleanly','owned-accepted-after-HTTP',r=>{ r.state.events.at(-1).clean=false; }],
    ['accepted socket omits wire reply','owned-accepted-after-HTTP',r=>{ r.peer.messages=[]; }],
    ['accepted socket omits wire Close','owned-accepted-after-HTTP',r=>{ r.peer.closes=[]; }],
    ['streamed response treated as already connected','owned-streamed-before-HTTP-end',r=>{ r.state.returnedState=1; }],
    ['streamed response loses body bytes','owned-streamed-before-HTTP-end',r=>{ r.response.text='ended'; }],
    ['streamed response loses live connection','owned-streamed-before-HTTP-end',r=>{ r.state.events.splice(1,2); }],
]) test(`HTTP ownership rejects ${name}`, () => {
    for (const mode of ['workerd','single-worker','static-pool','managed']) {
        const result=ownershipSample(scenario,mode);
        mutate(result);
        assert.throws(()=>assertOwnership(scenario,result,mode),mode);
    }
});

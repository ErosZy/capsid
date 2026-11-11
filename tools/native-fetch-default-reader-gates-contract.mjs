import assert from 'node:assert/strict';

export const plan = [7,4089,...Array(66).fill(4096),17];
export const total = 274449;
function fnv(bytes){let n=2166136261;for(const byte of bytes)n=Math.imul(n^byte,16777619)>>>0;return n.toString(16).padStart(8,'0');}
export function assertGatedDefaultReaderState(value,mode,state,token){
    assert.equal(value.token,token);assert.equal(value.status,200);assert.equal(value.bodyUsedBefore,false);assert.equal(value.bodyUsedAfter,true);
    if(mode==='buffer-all-control'){
        assert.equal(state.phaseTimedOut,0);assert.deepEqual(state.acks,[]);
        assert.equal(state.ready,true);assert.equal(state.eof,false);assert.equal(state.produced,7);
        if (value.error === null) {
            // A close-delimited raw response can legally end after the watchdog
            // closes it. The gate still proves it never delivered the full body.
            assert.equal(value.unexpectedWholeBodyCompletion,true);
            assert.equal(value.wholeBodyBytes,7);assert.ok(value.wholeBodyBytes<total);
        } else {assert.ok(!value.unexpectedWholeBodyCompletion);}
        return;
    }
    assert.equal(value.lockedAfterRelease,false);assert.equal(value.error,null);assert.equal(value.invalidBytes,0);assert.equal(state.phaseTimedOut,null);assert.equal(state.peerError,null);
    const count=mode==='cancel-after-first'?1:plan.length;
    assert.equal(value.phaseReadCounts.length,count);assert.ok(state.ready);
    let position=0,previous=-1;const phaseSizes=Array(count).fill(0),phaseCounts=Array(count).fill(0);
    for(const row of value.reads){
        assert.ok(row.phase===previous||row.phase===previous+1);previous=row.phase;
        assert.ok(row.phase>=0&&row.phase<count);assert.equal(row.done,false);assert.equal(row.closedAfterRead,'pending');
        assert.ok(row.value.length>0);phaseSizes[row.phase]+=row.value.length;phaseCounts[row.phase]++;
        const bytes=Buffer.alloc(row.value.length);for(let i=0;i<bytes.length;i++)bytes[i]=((position+i)*29+17)%251;
        assert.equal(row.value.type,'Uint8Array');assert.equal(row.value.valueHash,fnv(bytes));
        assert.ok(row.value.offset+bytes.length<=row.value.backingLength);
        assert.deepEqual(row.value.head,[...bytes.subarray(0,8)]);assert.deepEqual(row.value.tail,[...bytes.subarray(-8)]);position+=bytes.length;
    }
    assert.deepEqual(phaseSizes,plan.slice(0,count));assert.deepEqual(value.phaseReadCounts,phaseCounts);
    assert.equal(value.total,position);assert.ok(state.dataResponseClosed);assert.equal(state.openSockets,0);
    if(mode==='stream'){
        assert.equal(position,total);assert.deepEqual(value.eof,{done:true,value:{type:'undefined'}});assert.equal(value.closedAfterEof,'fulfilled');
        assert.deepEqual(state.acks,plan.map((_,index)=>index));assert.equal(state.eof,true);assert.equal(state.produced,total);
        assert.deepEqual(state.events.filter(e=>['emit','ack','eof'].includes(e.kind)).map(e=>[e.kind,e.part??null]),
            [...plan.flatMap((_,index)=>[['emit',index],['ack',index]]),['eof',null]]);
    }else{assert.equal(value.closedAfterCancel,'fulfilled');assert.deepEqual(state.acks,[]);assert.equal(state.eof,false);assert.equal(state.produced,7);}
}

// Recheck the full report independently of the runner's per-case counters.
// A positive reader result must be accompanied by a real gated source, closed
// connection, healthy next request and natural native process exit.
export function assertGatedDefaultReaderReport(report, {transport, fixtureSHA256}) {
    assert.ok(transport === 'ordinary' || transport === 'upgrade');
    assert.equal(report.transport, transport);
    assert.equal(report.complete, true);
    assert.equal(report.status, 'complete');
    assert.deepEqual(report.partPlan, [7,4089,...Array(66).fill(4096),17]);
    assert.equal(report.totalBodyBytes, 274449);
    assert.equal(report.phaseDeadlineMilliseconds, 2000);
    assert.equal(report.originalRequestDeadlineMilliseconds, 30000);
    assert.equal(report.sourceEndWithheldUntilLastReadAcknowledgment, true);
    assert.equal(report.programIdenticalAcrossNativeAndPrimary, true);
    assert.match(fixtureSHA256, /^[0-9a-f]{64}$/);
    const expected = ['raw/stream', 'raw/cancel-after-first', 'raw/buffer-all-control',
        'gzip/stream', 'gzip/cancel-after-first', 'gzip/buffer-all-control'];
    assert.deepEqual(report.cases.map(value => `${value.bodyKind}/${value.mode}`), expected);
    for (const entry of report.cases) {
        assert.equal(entry.failure, null);
        assert.deepEqual(Object.keys(entry.roles).sort(), ['native', 'primary']);
        for (const [name, row] of Object.entries(entry.roles)) {
            assert.equal(row.failure, null);
            assert.equal(row.cleanupFailure, undefined);
            assert.equal(row.httpStatus, 200);
            assert.equal(row.programSHA256, fixtureSHA256);
            assert.deepEqual(row.healthyReuse, {status:200,text:'healthy'});
            assert.equal(row.shutdown.forced, false);
            if (name === 'native') assert.equal(row.shutdown.code, 0);
            assert.doesNotMatch(row.stderr, /ERROR: AddressSanitizer|LeakSanitizer:|runtime error:|AddressSanitizer:DEADLYSIGNAL/);
            assert.equal(row.publicState.mode, entry.mode);
            assert.equal(row.publicState.bodyKind, entry.bodyKind);
            assert.equal(row.sourceBeforeBackendStop.openSockets, 0);
            assert.equal(row.sourceBeforeBackendStop.dataResponseClosed, true);
            assertGatedDefaultReaderState(row.publicState, entry.mode, row.sourceBeforeBackendStop, entry.token);
        }
        assert.equal(entry.roles.native.probeQuery, entry.roles.primary.probeQuery);
        if (entry.mode === 'buffer-all-control') {
            assert.equal(entry.actualWholeBodyWaitControlRejectedBothRuntimes, true);
        } else {
            assert.equal(entry.fullPublicStatesMatch, true);
            assert.deepEqual(entry.roles.native.publicState, entry.roles.primary.publicState,
                'complete read values, full backing hashes, closed, cancel and release states');
        }
    }
    return {complete:true, cases:6, fullPublicMatches:4, wholeBodyWaitControls:2,
        sourceReadGates:69, bodyBytes:274449, transport};
}

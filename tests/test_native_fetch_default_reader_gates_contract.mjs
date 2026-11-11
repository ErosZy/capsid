import assert from 'node:assert/strict';
import {test} from 'node:test';
import {assertGatedDefaultReaderReport} from '../tools/native-fetch-default-reader-gates-contract.mjs';

const fixtureSHA256 = 'a'.repeat(64);
const plan = [7,4089,...Array(66).fill(4096),17];
const hash = bytes => {
    let value = 2166136261;
    for (const byte of bytes) value = Math.imul(value ^ byte, 16777619) >>> 0;
    return value.toString(16).padStart(8,'0');
};

function golden(transport) {
    const report = {transport, complete:true,status:'complete',partPlan:[...plan],totalBodyBytes:274449,
        phaseDeadlineMilliseconds:2000,originalRequestDeadlineMilliseconds:30000,
        sourceEndWithheldUntilLastReadAcknowledgment:true,programIdenticalAcrossNativeAndPrimary:true,cases:[]};
    for (const bodyKind of ['raw','gzip']) for (const mode of ['stream','cancel-after-first','buffer-all-control']) {
        const token = `${transport}-${bodyKind}-${mode}`;
        const value = {token,bodyKind,mode,status:200,bodyUsedBefore:false,bodyUsedAfter:true,
            lockedAfterRelease:false,error:null,invalidBytes:0,total:0,reads:[],phaseReadCounts:[]};
        const source = {ready:true,openSockets:0,dataResponseClosed:true,phaseTimedOut:null,
            peerError:null,eof:false,produced:7,acks:[],events:[]};
        if (mode === 'buffer-all-control') {
            source.phaseTimedOut = 0;
            if (bodyKind === 'raw') Object.assign(value,{unexpectedWholeBodyCompletion:true,wholeBodyBytes:7});
            else value.error = {name:'TypeError',message:'Gzip compressed stream ended prematurely.'};
        } else {
            const count = mode === 'stream' ? 69 : 1;
            for (let phase = 0; phase < count; phase++) {
                const bytes = Uint8Array.from({length:plan[phase]},(_,index)=>((value.total+index)*29+17)%251);
                const backing = new Uint8Array(4096); backing.set(bytes);
                value.reads.push({phase,done:false,closedAfterRead:'pending',value:{type:'Uint8Array',offset:0,
                    length:bytes.length,backingLength:4096,valueHash:hash(bytes),backingHash:hash(backing),
                    head:[...bytes.slice(0,8)],tail:[...bytes.slice(-8)]}});
                value.phaseReadCounts.push(1); value.total += bytes.length;
                source.events.push({kind:'emit',part:phase});
                if (mode === 'stream') {source.acks.push(phase);source.events.push({kind:'ack',part:phase});}
            }
            if (mode === 'stream') {
                value.eof = {done:true,value:{type:'undefined'}};value.closedAfterEof = 'fulfilled';
                source.eof = true;source.produced = 274449;source.events.push({kind:'eof'});
            } else value.closedAfterCancel = 'fulfilled';
        }
        const role = {failure:null,httpStatus:200,programSHA256:fixtureSHA256,probeQuery:token,
            publicState:value,sourceBeforeBackendStop:source,healthyReuse:{status:200,text:'healthy'},
            shutdown:{code:0,forced:false},stderr:''};
        report.cases.push({bodyKind,mode,token,failure:null,roles:{primary:structuredClone(role),native:role},
            ...(mode === 'buffer-all-control' ? {actualWholeBodyWaitControlRejectedBothRuntimes:true} : {fullPublicStatesMatch:true})});
    }
    return report;
}

for (const transport of ['ordinary','upgrade']) {
    test(`independent complete ${transport} case inventory`,()=>{
        assert.deepEqual(assertGatedDefaultReaderReport(golden(transport),{transport,fixtureSHA256}),
            {complete:true,cases:6,fullPublicMatches:4,wholeBodyWaitControls:2,sourceReadGates:69,bodyBytes:274449,transport});
    });
    const mutations = [
        ['missing gzip',r=>r.cases.splice(3)],
        ['duplicate case',r=>r.cases[3]=structuredClone(r.cases[0])],
        ['changed ordering',r=>r.cases.reverse()],
        ['skip',r=>r.cases[0].failure='skipped'],
        ['incomplete',r=>r.complete=false],
        ['raised gate timeout',r=>r.phaseDeadlineMilliseconds=2001],
        ['raised request timeout',r=>r.originalRequestDeadlineMilliseconds=30001],
        ['shorter body',r=>r.totalBodyBytes=65536],
        ['shorter inventory',r=>r.partPlan.pop()],
        ['other transport',r=>r.transport='other'],
        ['early EOF allowed',r=>r.sourceEndWithheldUntilLastReadAcknowledgment=false],
        ['changed guest allowed',r=>r.programIdenticalAcrossNativeAndPrimary=false],
        ['no native',r=>delete r.cases[0].roles.native],
        ['extra role',r=>r.cases[0].roles.synthetic=structuredClone(r.cases[0].roles.native)],
        ['changed guest hash',r=>r.cases[0].roles.native.programSHA256='b'.repeat(64)],
        ['other probe query',r=>r.cases[0].roles.native.probeQuery='other'],
        ['forced cleanup',r=>r.cases[0].roles.native.shutdown.forced=true],
        ['native leak exit',r=>r.cases[0].roles.native.shutdown.code=1],
        ['native stderr sanitizer',r=>r.cases[0].roles.native.stderr='ERROR: AddressSanitizer'],
        ['failed healthy reuse',r=>r.cases[0].roles.native.healthyReuse.status=503],
        ['cleanup failure',r=>r.cases[0].roles.native.cleanupFailure='deadline'],
        ['live socket',r=>r.cases[0].roles.native.sourceBeforeBackendStop.openSockets=1],
        ['source not closed',r=>r.cases[0].roles.native.sourceBeforeBackendStop.dataResponseClosed=false],
        ['wrong byte hash',r=>r.cases[0].roles.native.publicState.reads[0].value.valueHash='00000000'],
        ['encoded gzip instead of identity bytes',r=>r.cases[3].roles.native.publicState.reads[0].value.head=[31,139,8,0]],
        ['changed complete backing hash',r=>r.cases[0].roles.native.publicState.reads[0].value.backingHash='other'],
        ['changed backing size',r=>r.cases[0].roles.native.publicState.reads[0].value.backingLength=65536],
        ['closed before read',r=>r.cases[0].roles.native.publicState.reads[0].closedAfterRead='fulfilled'],
        ['unreleased reader',r=>r.cases[0].roles.native.publicState.lockedAfterRelease=true],
        ['timed out positive',r=>r.cases[0].roles.native.sourceBeforeBackendStop.phaseTimedOut=3],
        ['missing ack',r=>r.cases[0].roles.native.sourceBeforeBackendStop.acks.pop()],
        ['source produces before ack',r=>r.cases[0].roles.native.sourceBeforeBackendStop.events.reverse()],
        ['no positive equality',r=>r.cases[0].fullPublicStatesMatch=false],
        ['no real negative control',r=>r.cases[2].actualWholeBodyWaitControlRejectedBothRuntimes=false],
        ['whole body negative faked complete',r=>r.cases[2].roles.native.publicState.wholeBodyBytes=274449],
        ['whole body negative fake source',r=>r.cases[2].roles.native.sourceBeforeBackendStop.eof=true],
        ['whole body negative no actual watchdog',r=>r.cases[2].roles.native.sourceBeforeBackendStop.phaseTimedOut=null],
    ];
    for (const [name,mutate] of mutations) test(`${transport} rejects ${name}`,()=>{
        const value = golden(transport); mutate(value);
        assert.throws(()=>assertGatedDefaultReaderReport(value,{transport,fixtureSHA256}));
    });
}

import assert from 'node:assert/strict';
import { test as nodeTest, after } from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { gzipSync } from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { compareReports, deviations, readLock, installReference, verifyBinary, root, fixtureIdentities } from '../tools/workerd-reference.mjs';
import { validateGateRun, requiredTestsFor } from '../tools/run-workerd-ws-gate.mjs';
import {referencePlatform, referenceBackendModes} from '../tools/workerd-platform.mjs';
import { createHash } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';
import { serialize, deserialize } from 'node:v8';
import {prepareMutationRunner, mutationHandlerCase} from '../tools/workerd-mutation-contract.mjs';
import {readFetchProxySample} from './helpers/ws_fetch_proxy_sample.mjs';
import {readFetchBoundarySample} from './helpers/ws_fetch_boundary_sample.mjs';
import {readFetchTransitionSample} from './helpers/ws_fetch_transition_sample.mjs';
import {readFetchBYOBSample} from './helpers/ws_fetch_byob_sample.mjs';
import {readReaderIntrinsicSample} from './helpers/ws_reader_intrinsic_sample.mjs';
import {readNativeReaderLifecycleSample} from './helpers/ws_native_reader_lifecycle_sample.mjs';
import {readReaderScopeSample} from './helpers/ws_reader_scope_sample.mjs';
import {readStandardReaderSample} from './helpers/ws_standard_reader_sample.mjs';
import {readOrdinaryFetchSamples} from './helpers/ws_ordinary_fetch_sample.mjs';
import {readHttpCodecSamples} from './helpers/ws_http_codec_sample.mjs';

// Every group independently checks the complete frozen registration list.
const inventory=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-gate-control-inventory.json',import.meta.url),'utf8'));
assert.equal(inventory.schema,'capsid-workerd-gate-control-inventory-v1');assert.equal(inventory.cases.length,296);
assert.equal(new Set(inventory.cases).size,296);
const controlShard=process.env.CAPSID_WS_GATE_CONTROL_SHARD??'all';
assert.ok(['all','primary','archives','extensions'].includes(controlShard),'known gate control shard');
const declaredControls=[];let registeredControls=0;
function test(name,...args){
    const index=declaredControls.length;assert.equal(name,inventory.cases[index],'complete independent gate control inventory');
    declaredControls.push(name);
    if(controlShard==='all'||(index<114?'primary':index<231?'archives':'extensions')===controlShard){registeredControls++;return nodeTest(name,...args);}
}

// Reuse immutable construction templates; every mutant receives a fresh
// deep clone. The complete validators and archive I/O still run per case.
const matrixTemplates = new Map(), archiveTemplates = new Map();
const constructionTemplateBytes=new WeakMap();
function assertTemplateCloneGraph(source,clone,forward=new Map(),reverse=new Map()){
    if(source===null||typeof source!=='object')return;
    assert.notEqual(source,clone,'cloned objects are independent');
    if(forward.has(source)){assert.equal(forward.get(source),clone,'shared references survive');return;}
    assert.ok(!reverse.has(clone),'distinct source objects remain distinct');
    forward.set(source,clone);reverse.set(clone,source);
    assert.deepEqual(Reflect.ownKeys(source),Reflect.ownKeys(clone),'all template property keys survive');
    for(const key of Reflect.ownKeys(source))assertTemplateCloneGraph(source[key],clone[key],forward,reverse);
}
function cloneConstructionTemplate(value){
    let bytes=constructionTemplateBytes.get(value);
    if(bytes===undefined){
        bytes=serialize(value);
        const decoded=deserialize(bytes);
        assert.deepEqual(decoded,structuredClone(value),'template clone retains every current value');
        assertTemplateCloneGraph(value,decoded);
        const independent=deserialize(bytes);
        assert.deepEqual(independent,decoded,'repeated clones retain every current value');
        assertTemplateCloneGraph(decoded,independent);
        constructionTemplateBytes.set(value,bytes);
        return decoded;
    }
    return deserialize(bytes);
}

function matrix(platform = 'linux') {
    assert.ok(['linux', 'win32'].includes(platform));
    if (matrixTemplates.has(platform)) return cloneConstructionTemplate(matrixTemplates.get(platform));
    const lock = readLock(new URL(platform === 'win32' ? './fixtures/workerd-ws/reference-lock-windows.json'
        : './fixtures/workerd-ws/reference-lock.json', import.meta.url));
    const fixtures = ['api-compat.mjs', 'close-boundary.mjs', 'disconnect-mid-stream.mjs',
        'double-accept.mjs', 'echo.mjs', 'fanout.mjs', 'fragmented.mjs', 'invalid-utf8.mjs',
        'lifecycle.mjs', 'ping-flood.mjs', 'response.mjs', 'semantics.mjs', 'server-close.mjs',
        'slow-consumer.mjs', 'transport.mjs'].sort()
        .map(fixture => ({ fixture, sha256: 'a'.repeat(64) }));
    const common = { schema:'capsid-ws-fixtures-v1', complete: true, runner_sha256: 'b'.repeat(64), lock, fixtures,
        client: { node: lock.client.node_version, undici: lock.client.undici_version },
        results: fixtures.map(x => ({ fixture: x.fixture, passed: true, assertions: 1,
            shutdown: { code: 0, signal: null, forced: false },
            observation: { received: 'echo', code: 1000 } })) };
    common.results.find(x => x.fixture === 'api-compat.mjs').observation.acceptedEndpointStatus = 101;
    common.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerThrows = {
        c17: {wire: {hex: '8106717565756564', frames: [{opcode: 1, payload: 'cXVldWVk'}],
            peerTerminated: true, completion: 'peer-ended', transportError: null},
            client: {seen: [], close: {code: 1006, reason: '', wasClean: false}}},
        c18: {wire: {hex: '8108612d6265666f7265880603e8646f6e65', frames: [
            {opcode: 1, payload: 'YS1iZWZvcmU='}, {opcode: 8, payload: 'A+hkb25l'},
        ], peerTerminated: true, completion: 'peer-reset', transportError: 'ECONNRESET'},
            client: {seen: ['a-before'], close: {code: 1000, reason: 'done', wasClean: true}}},
    };
    common.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerErrors = JSON.parse(
        fs.readFileSync(new URL('./fixtures/workerd-handler-error-report.json', import.meta.url), 'utf8')).observations;
    common.results.find(x => x.fixture === 'fanout.mjs').observation = {
        identities:[0,1].map(index=>({instance:'11111111-1111-4111-8111-111111111111',
            connection:`22222222-2222-4222-8222-22222222222${index}`})),
        sender:'fanout:blocked',peerMessages:[],survivor:'fanout:blocked',
        closes:[0,1].map(()=>({code:1000,reason:'',wasClean:true})),
    };
    common.results.find(x => x.fixture === 'response.mjs').observation = JSON.parse(
        fs.readFileSync(new URL('./fixtures/workerd-response-report.json',import.meta.url))).observation;
    common.results.find(x => x.fixture === 'close-boundary.mjs').observation.invalidClose = { code: 999, reason: 'in' };
    common.results.find(x => x.fixture === 'ping-flood.mjs').observation.pongs = ['ping-000', 'ping-049'];
    Object.assign(common.results.find(x => x.fixture === 'slow-consumer.mjs').observation,
        { frames: 128, bytes: 8388608, closeFrame: 1000, peerClosed: true, completionMessage: 'slow-burst-done' });
    const reference = { ...structuredClone(common), schema:'capsid-workerd-ws-reference-v1', binary_sha256: lock.binary_sha256 };
    const candidates = (platform === 'win32' ? ['single-worker', 'static-pool'] : ['single-worker', 'static-pool', 'managed']).map(mode => ({
        ...structuredClone(common), mode, reference_parity:false, host_sha256: 'c'.repeat(64), worker_sha256: 'd'.repeat(64),
        results: common.results.map(x => ({ ...structuredClone(x), deviation: deviations[x.fixture] ?? null })),
    }));
    for (const candidate of candidates) {
        candidate.results.find(x => x.fixture === 'close-boundary.mjs').observation.invalidClose = { code: 1002, reason: '' };
        Object.assign(candidate.results.find(x => x.fixture === 'slow-consumer.mjs').observation,
            { frames: 7, bytes: 7 * 65536, closeFrame: null, completionMessage: null });
    }
    const result = { reference, candidates, expected: { lock, fixtures, runner: common.runner_sha256,
        host: 'c'.repeat(64), worker: 'd'.repeat(64) } };
    matrixTemplates.set(platform, result);
    return cloneConstructionTemplate(result);
}

for(const [name,mutate] of [
    ['missing BYOB report',m=>{delete m.files['fetch-byob-readers-managed'];}],
    ['stale BYOB source',m=>{m.files['fetch-byob-readers-reference'].inputs.fixture='0'.repeat(64);}],
    ['stale BYOB worker',m=>{m.files['fetch-byob-readers-static-pool'].binaries.worker='0'.repeat(64);}],
    ['incomplete BYOB report',m=>{m.files['fetch-byob-readers-reference'].complete=false;}],
    ['same BYOB case omitted everywhere',m=>{for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-byob-'))r.cases.pop();}],
    ['same nonempty BYOB EOF done flag everywhere',m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-byob-')){
            const v=r.cases.find(v=>v.kind==='native-getReader-Uint16Array-min3-eof');if(!v)continue;const s=JSON.parse(v.observed.text);
            s.state.read.value.done=true;v.observed.text=JSON.stringify(s);
        }
    }],
    ['same native BYOB typed view everywhere',m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-byob-')){
            const v=r.cases.find(v=>v.kind==='native-direct-Uint16Array-min2-eof');if(!v)continue;const s=JSON.parse(v.observed.text);
            s.state.read.value.type='Uint16Array';v.observed.text=JSON.stringify(s);
        }
    }],
    ['missing extended BYOB shard',m=>{delete m.files['fetch-byob-views-brands-reference'];}],
    ['missing standard close timing shard',m=>{delete m.files['fetch-byob-standard-close-reference'];}],
    ['missing standard EOF and cancellation shard',m=>{delete m.files['fetch-byob-standard-edges-reference'];}],
    ['same missing standard EOF diagnostic everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-byob-standard-edges-')){
            report.cases.find(v=>v.kind==='standard-getReader-Uint16Array-min3-eof').stderr='';
        }
    }],
    ['missing native body shard',m=>{delete m.files['fetch-byob-bodies-reference'];}],
    ['same native body shard omitted everywhere',m=>{for(const key of Object.keys(m.files))if(key.startsWith('fetch-byob-bodies-'))delete m.files[key];}],
    ...[
        ['same native DefaultReader backing error everywhere',s=>{s.shapes[0][3]=512;}],
        ['same offset BYOB EOF error everywhere',s=>{s.shapes.at(-1)[1]=16;}],
        ['same truncated cross-window body everywhere',s=>{s.sha256='0'.repeat(64);}],
    ].map(([name,mutate],index)=>[name,m=>{
        const kind=index===0?'body-getReader-default-small':'body-getReader-minimum-large';
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-byob-bodies-')){
            const value=r.cases.find(value=>value.kind===kind),document=JSON.parse(value.observed.text);
            mutate(document.state);value.observed.text=JSON.stringify(document);
        }
    }]),
    ...[
        ['same Float16 width error everywhere','native-getReader-Float16Array-min2-eof',s=>{s.afterCall.bufferBytes=32;}],
        ['same wrong cancellation reason everywhere','brand-getReader-byob-held',s=>{s.read.error.message='invalid/'+s.challenge;}],
        ['same asynchronous receiver rejection everywhere','brand-direct-default-held',s=>{s.attempts[0].synchronous=false;}],
        ['same released receiver check omitted everywhere','brand-reflect-byob-held',s=>{s.afterReleaseAttempts.pop();}],
    ].map(([name,kind,mutate])=>[name,m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-byob-views-brands-')){
            const v=r.cases.find(v=>v.kind===kind);assert.ok(v);const d=JSON.parse(v.observed.text);mutate(d.state);v.observed.text=JSON.stringify(d);
        }
    }]),
    ['same BYOB credit bytes missing everywhere',m=>{
        let changed=0;
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-byob-')){
            const value=r.cases.find(value=>value.upstreamBeforeStop.records.length>0);
            if(!value){assert.ok(r.cases.every(value=>value.kind.startsWith('standard-')||value.kind.startsWith('validation-')));continue;}
            const source=value.upstreamBeforeStop.records[0];assert.ok(source.sentChunks.length>0);
            source.sentChunks.pop();changed++;
        }
        assert.equal(changed,20,'five upstream shards on the reference and every Linux backend are mutated');
    }],
])test('required gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

test('complete matching reports and documented exceptions pass', () => {
    const { reference, candidates, expected } = matrix();
    assert.equal(compareReports(reference, candidates, expected).complete, true);
});

for (const [name, mutate] of [
    ['missing reference schema', m => { delete m.reference.schema; }],
    ['wrong reference schema', m => { m.reference.schema = 'capsid-ws-fixtures-v1'; }],
    ['wrong candidate schema', m => { m.candidates[0].schema = 'unknown'; }],
    ['false parity claim', m => { m.candidates[0].reference_parity = true; }],
    ['missing parity claim', m => { delete m.candidates[0].reference_parity; }],
    ['partial matrix', m => { m.reference.complete = false; }],
    ['missing fixture', m => { m.candidates[0].results.pop(); }],
    ['duplicate fixture', m => { m.candidates[0].results[1] = m.candidates[0].results[0]; }],
    ['zero assertions', m => { m.candidates[0].results[0].assertions = 0; }],
    ['skip', m => { m.reference.results[0].skipped = 'unavailable'; }],
    ['missing observation', m => { delete m.candidates[0].results[0].observation; }],
    ['throw wire observations removed from every report', m => {
        for (const report of [m.reference, ...m.candidates])
            delete report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerThrows;
    }],
    ['handler errors removed from every report', m => {
        for (const report of [m.reference, ...m.candidates])
            delete report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerErrors;
    }],
    ['new object case omitted from both sides', m => {
        for (const report of [m.reference, ...m.candidates])
            delete report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerErrors['c17/object-primitive'];
    }],
    ['shared object reference identically corrupted on both sides', m => {
        for (const report of [m.reference, ...m.candidates])
            report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerErrors['c17/object-cycle']
                .events.at(-1).objectGraph.properties.right.value = {ref:7};
    }],
    ['identically omitted trusted error on every backend', m => {
        for (const report of [m.reference, ...m.candidates])
            report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerErrors['c17/Error'].events.pop();
    }],
    ['identically early CLOSED during microtasks on every backend', m => {
        for (const report of [m.reference, ...m.candidates])
            report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerErrors['c17/Error'].events[1].readyState = 3;
    }],
    ['identically dropped accepted late send on every backend', m => {
        for (const report of [m.reference, ...m.candidates])
            report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerErrors['c17/Error'].wire.hex = '8106717565756564';
    }],
    ['identically dropped accepted sends on every backend', m => {
        for (const report of [m.reference, ...m.candidates]) {
            const wire = report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerThrows.c17.wire;
            wire.hex = ''; wire.frames = [];
        }
    }],
    ['identically dropped Close on every backend', m => {
        for (const report of [m.reference, ...m.candidates]) {
            const wire = report.results.find(x => x.fixture === 'api-compat.mjs').observation.handlerThrows.c18.wire;
            wire.hex = '8108612d6265666f7265'; wire.frames.pop();
        }
    }],
    ['null observations on both sides', m => {
        for (const r of [m.reference, ...m.candidates]) r.results.find(x => x.fixture === 'echo.mjs').observation = null;
    }],
    ['empty observations on both sides', m => {
        for (const r of [m.reference, ...m.candidates]) r.results.find(x => x.fixture === 'echo.mjs').observation = {};
    }],
    ['fanout without instance identity on every backend', m => {
        for (const report of [m.reference, ...m.candidates])
            report.results.find(x => x.fixture === 'fanout.mjs').observation = {
                sender: 'fanout:blocked', peerMessages: [], survivor: 'fanout:blocked',
                closes: [0, 1].map(() => ({code: 1000, reason: '', wasClean: true})),
            };
    }],
    ['fixture removed from discovery and every report', m => {
        m.expected.fixtures.pop();
        for (const r of [m.reference, ...m.candidates]) { r.fixtures.pop(); r.results.pop(); }
    }],
    ['wrong close code inside declared fixture deviation', m => {
        m.candidates[0].results.find(r => r.fixture === 'close-boundary.mjs').observation.invalidClose.code = 1000;
    }],
    ['unrelated behavior inside declared fixture deviation', m => {
        m.candidates[0].results.find(r => r.fixture === 'close-boundary.mjs').observation.received = 'corrupted';
    }],
    ['empty pong trace', m => { m.candidates[0].results.find(r => r.fixture === 'ping-flood.mjs').observation.pongs = []; }],
    ['reordered pongs', m => { m.candidates[0].results.find(r => r.fixture === 'ping-flood.mjs').observation.pongs.reverse(); }],
    ['corrupted pong payload', m => { m.candidates[0].results.find(r => r.fixture === 'ping-flood.mjs').observation.pongs[0] = 'corrupt'; }],
    ['unbounded burst accepted', m => { m.candidates[0].results.find(r => r.fixture === 'slow-consumer.mjs').observation.frames = 128; }],
    ['zero burst delivery', m => { m.candidates[0].results.find(r => r.fixture === 'slow-consumer.mjs').observation.frames = 0; }],
    ['burst did not terminate', m => { m.candidates[0].results.find(r => r.fixture === 'slow-consumer.mjs').observation.peerClosed = false; }],
    ['missing shutdown', m => { delete m.candidates[0].results[0].shutdown; }],
    ['forced shutdown', m => { m.reference.results[0].shutdown.forced = true; }],
    ['abnormal Host exit', m => { m.candidates[0].results[0].shutdown.code = 1; }],
    ['behavior mismatch', m => { m.candidates[0].results[0].observation.code = 1006; }],
    ['accepted endpoint regresses to pre-commit error', m => { m.candidates[0].results[0].observation.acceptedEndpointStatus = 500; }],
    ['Response fixture removed from discovery and every report', m => {
        m.expected.fixtures=m.expected.fixtures.filter(x=>x.fixture!=='response.mjs');
        for(const report of [m.reference,...m.candidates]) {
            report.fixtures=report.fixtures.filter(x=>x.fixture!=='response.mjs');
            report.results=report.results.filter(x=>x.fixture!=='response.mjs');
        }
    }],
    ['identical wrong Response body behavior on every backend', m => {
        for(const report of [m.reference,...m.candidates]) {
            const value=report.results.find(x=>x.fixture==='response.mjs').observation.constructors;
            value['body-buffer-spoof-empty']=structuredClone(value['body-buffer-empty']);
        }
    }],
    ['identical pre-commit coupling error on every backend', m => {
        for(const report of [m.reference,...m.candidates]) report.results.find(x=>x.fixture==='response.mjs').observation.aborted[0].wire.status=500;
    }],
    ['identically dropped Response quota round on every backend', m => {
        for(const report of [m.reference,...m.candidates]) report.results.find(x=>x.fixture==='response.mjs').observation.aborted.pop();
    }],
    ['undeclared deviation', m => { m.candidates[0].results[0].deviation = 'anything'; }],
    ['fixture digest', m => { m.candidates[0].fixtures[0].sha256 = 'wrong'; }],
    ['runner digest', m => { m.reference.runner_sha256 = 'wrong'; }],
    ['client', m => { m.reference.client.node = 'v0.0.0'; }],
    ['compatibility date', m => { m.reference.lock.compatibility_date = '2000-01-01'; }],
    ['reference binary', m => { m.reference.binary_sha256 = 'wrong'; }],
    ['Host binary', m => { m.candidates[0].host_sha256 = 'wrong'; }],
    ['Worker binary', m => { m.candidates[0].worker_sha256 = 'wrong'; }],
    ['missing mode', m => { m.candidates.pop(); }],
    ['duplicate mode', m => { m.candidates[1].mode = m.candidates[0].mode; }],
]) test(`reject ${name}`, () => {
    const m = matrix();
    mutate(m);
    assert.throws(() => compareReports(m.reference, m.candidates, m.expected));
});

test('missing reference fails before CTest and invalidates an earlier successful summary', () => {
    const env = { ...process.env };
    delete env.CAPSID_WORKERD_REF;
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-gate-stale-summary-'));
    try {
        const summary = path.join(directory, 'summary.json');
        fs.writeFileSync(summary, JSON.stringify({complete:true, run:'previous-run'}));
        assert.throws(() => execFileSync(process.execPath,
            [path.join(root, 'tools/run-workerd-ws-gate.mjs'), 'unused-build', directory],
            { env, stdio: 'pipe' }), error => error.status === 1 &&
                String(error.stderr).includes('CAPSID_WORKERD_REF is required'));
        const failed = JSON.parse(fs.readFileSync(summary, 'utf8'));
        assert.equal(failed.complete, false, 'a failed attempt cannot leave a passing current summary');
        assert.equal(failed.status, 'failed');
        assert.match(failed.error, /CAPSID_WORKERD_REF is required/);
        assert.equal(fs.readdirSync(directory).filter(n=>n.startsWith('run-')).length,0,
            'preflight failure did not launch CTest');
    } finally { fs.rmSync(directory, {recursive:true,force:true}); }
});

test('deleting a fixture file cannot shrink the required manifest', () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-fixture-manifest-'));
    try {
        for (const {fixture} of fixtureIdentities()) fs.writeFileSync(path.join(dir, fixture), '// fixture');
        assert.equal(fixtureIdentities(dir).length, 15);
        fs.unlinkSync(path.join(dir, 'transport.mjs'));
        assert.throws(() => fixtureIdentities(dir), /required manifest/);
    } finally { fs.rmSync(dir, {recursive: true, force: true}); }
});

test('installer rejects wrong compressed digest and never installs it',
    { skip: !['linux', 'win32'].includes(process.platform) || process.arch !== 'x64' }, async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-workerd-lock-'));
    try {
        const asset = path.join(dir, 'asset.gz'), output = path.join(dir, 'workerd');
        fs.writeFileSync(asset, gzipSync('wrong binary'));
        await assert.rejects(installReference(output, readLock(), asset), /compressed workerd digest/);
        assert.equal(fs.existsSync(output), false);
        fs.writeFileSync(output, 'wrong binary');
        assert.throws(() => verifyBinary(output, readLock()), /executable digest/);
    } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

// The archive verifier must read the actual mutation report, independently of
// a successful CTest XML entry. Keep this reviewed inventory separate from the
// producer's variants so deleting a variant cannot silently shrink the gate.
const mutationCases = [
    ['baseline', null],
    ['wire-code', /c03: actual wire close conversion/],
    ['wire-reason', /e-close: actual wire close conversion/],
    ['throw-send-dropped', /c17: throw wire contract exact bytes/],
    ['throw-close-corrupted', /c18: throw wire contract exact bytes/],
    ['handler-error-microtask-dropped', /c17\/Error: handler error contract exact microtask order/],
    ['handler-error-type-lost', /c17\/TypeError: handler error contract exact microtask order/],
    ['handler-error-custom-name-lost', /c17\/subclass: handler error contract exact microtask order/],
    ['handler-error-number-stringified', /c17\/number-message: handler error contract exact microtask order/],
    ['handler-error-message-getter-twice', /c17\/message-getter: handler error contract exact microtask order/],
    ['handler-error-replacement-timer-called', /c17\/override-timer: handler error contract exact microtask order/],
    ['handler-error-listener-write-dropped', /c17\/error-send: handler error contract exact accepted wire bytes/],
    ['handler-error-conversion-skipped', /c17\/error-send-object: handler error contract exact microtask order/],
    ['handler-error-getter-close-ignored', /c17\/getter-close: handler error contract exact microtask order/],
    ['handler-object-conversion-called', /c17\/object: handler error contract exact microtask order/],
    ['handler-object-shared-reference-lost', /c17\/object-cycle: handler error contract exact microtask order/],
    ['handler-object-error-prototype', /c17\/object-fake-error: handler error contract exact microtask order/],
    ['handler-dom-private-name-lost', /c17\/dom-exception: handler error contract exact microtask order/],
    ['handler-aggregate-alias-lost', /c17\/aggregate-data: handler error contract exact microtask order/],
    ['handler-error-getter-order-lost', /c17\/error-ordered-getters: handler error contract exact microtask order/],
    ['handler-error-original-stack-retained', /c17\/error-fixed-stack: handler error contract exact microtask order/],
    ['handler-error-diagnostic-unlinked', /c17\/name-getter-throws: handler error contract exact microtask order/],
    ['send-surrogate-normalized', /DOMString wire message 4/],
    ['close-surrogate-corrupted', /close reason uses USVString with the captured intrinsic/],
    ['getter-reads', /public event contract: CloseEvent\/reads/],
    ['error-payload', /refused dial carries the reference Error object/],
    ['event-phase', /public event contract: Event\/defaults/],
    ['runtime-trust', /refused dial carries the reference Error object/],
    ['handled-reason', /runtime event delivery matches the pinned default workerd profile/],
    ['message-source', /runtime event delivery matches the pinned default workerd profile/],
    ['cached-message-ports', /public event contract: MessageEvent\/ports-undefined/],
    ['listener-exception-swallowed', /listener contract: EventTarget\/exception/],
    ['listener-removal-ignored', /listener contract: EventTarget\/remove-next-once/],
    ['listener-option-reread', /listener contract: EventTarget\/option-read-order/],
    ['handler-order', /event handler contract: WebSocket\/open\/replace/],
    ['handler-clear-removes-listener', /event handler contract: MessagePort\/message\/clear/],
];

test('isolated fault programs keep the full baseline and bind each handler fault to its independent diagnostic',()=>{
    const source=fs.readFileSync(path.join(root,'tests/test_workerd_ws_fixtures.mjs'),'utf8');
    const baseline=prepareMutationRunner(source);
    assert.ok(baseline.includes('observation.handlerErrors = await observeHandlerErrors(port, routePrefix, assert);'));
    assert.ok(baseline.includes('assertHandlerErrorDiagnostics(observation.handlerErrors, server.stderr(), assert);'));
    for (const [name, diagnostic] of mutationCases) {
        const expected=diagnostic?.source.includes(': handler error contract')
            ? diagnostic.source.split(': handler error contract')[0].replaceAll('\\/','/') : null;
        assert.equal(mutationHandlerCase(name)??null,expected,name+' independent scope');
        const program=prepareMutationRunner(source,name);
        if(expected) assert.ok(program.includes('assert,'+JSON.stringify(expected)+')'),name+' exact target');
        else if(name!=='baseline') {
            assert.ok(!program.includes('observation.handlerErrors = await observeHandlerErrors('));
            assert.ok(program.includes('refused dial carries the reference Error object'));
        }
    }
    assert.throws(()=>prepareMutationRunner(source,'unknown-fault'),/known mutation execution scope/);
});

function archiveMatrix(platform = 'linux') {
    if (archiveTemplates.has(platform)) return cloneConstructionTemplate(archiveTemplates.get(platform));
    const m = matrix(platform);
    const interopSample=JSON.parse(fs.readFileSync(new URL('./fixtures/websocket-interop-report.json',import.meta.url))).report;
    const fanoutSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-fanout-report.json',import.meta.url))).report;
    const frameSample=JSON.parse(fs.readFileSync(new URL('./fixtures/websocket-frame-report.json',import.meta.url))).report;
    const ownerSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-owner-report.json',import.meta.url)));
    const asyncSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-async-callback-report.json',import.meta.url)));
    const budgetSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-async-budget-report.json',import.meta.url)));
    const resourceSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-fetch-resource-report.json',import.meta.url)));
    const upgradeSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-fetch-upgrade-report.json',import.meta.url)));
    const lifetimeSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-fetch-upgrade-lifetime-report.json',import.meta.url)));
    const proxySample=readFetchProxySample();
    const boundarySample=readFetchBoundarySample();
    const transitionSample=readFetchTransitionSample();
    const byobSample=readFetchBYOBSample();
    const readerIntrinsicSample=readReaderIntrinsicSample();
    const nativeReaderLifecycleSample=readNativeReaderLifecycleSample();
    const readerScopeSample=readReaderScopeSample();
    const standardReaderSample=readStandardReaderSample();
    const cryptoSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-crypto-report.json',import.meta.url)));
    const crossSample=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-cross-context-report.json',import.meta.url)));
    Object.assign(m.expected, {outboundRunner:'e'.repeat(64), mutationController:'f'.repeat(64),
        mutationContract:'1'.repeat(64), mutationRunner:'2'.repeat(64), runId:interopSample.runId,
        mutationRunners:Object.fromEntries(mutationCases.map(([name])=>[name,'2'.repeat(64)])),handlerErrorObserver:'3'.repeat(64),
        interopInputs:structuredClone(interopSample.inputs),interopClientLock:structuredClone(interopSample.clientLock),
        fanoutInputs:structuredClone(fanoutSample.inputs), frameInputs:structuredClone(frameSample.inputs),
        ownerInputs:structuredClone(ownerSample.inputs),asyncInputs:structuredClone(asyncSample.inputs),
        crossContextInputs:structuredClone(crossSample.inputs),fetchResourceInputs:structuredClone(resourceSample.inputs),
        ordinaryFetchInputs:structuredClone(readOrdinaryFetchSamples(m.expected.lock)[0].inputs),
        httpCodecInputs:structuredClone(readHttpCodecSamples(m.expected.lock)[0].inputs),
        fetchUpgradeInputs:structuredClone(upgradeSample.inputs),
        fetchUpgradeLifetimeInputs:structuredClone(lifetimeSample.inputs),
        fetchProxyInputs:structuredClone(proxySample.inputs),
        fetchBoundaryInputs:structuredClone(boundarySample.inputs),
        fetchTransitionInputs:structuredClone(transitionSample.inputs),
        fetchBYOBInputs:structuredClone(byobSample.inputs),
        readerIntrinsicInputs:structuredClone(readerIntrinsicSample.inputs),
        nativeReaderLifecycleInputs:structuredClone(nativeReaderLifecycleSample.inputs),
        readerScopeInputs:structuredClone(readerScopeSample.inputs),
        standardReaderInputs:structuredClone(standardReaderSample.inputs),
        readerIntrinsicSourceNames:{reference:'main','single-worker':'file:///qualification/intrinsics.mjs',
            'static-pool':'file:///qualification/intrinsics.mjs',managed:'file://orders/v1/bundle.mjs'},
        cryptoInputs:structuredClone(cryptoSample.inputs),
        asyncSourceNames:{reference:'main','single-worker':'file:///qualification/callbacks.mjs',
            'static-pool':'file:///qualification/callbacks.mjs',managed:'file://orders/v1/bundle.mjs'}});
    const outbound = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-outbound-reports.json', import.meta.url)));
    const files = {reference:m.reference};
    const portSample = JSON.parse(fs.readFileSync(new URL('./fixtures/message-port-order-reference.json', import.meta.url), 'utf8'));
    assert.equal(portSample.fixtureSHA256, createHash('sha256').update(fs.readFileSync(new URL('./fixtures/message-port-order.mjs', import.meta.url))).digest('hex'));
    m.expected.messagePortInputs = Object.fromEntries(['fixture', 'runner', 'helper', 'contract'].map(name => [name, '4'.repeat(64)]));
    m.expected.messagePortNode = '5'.repeat(64);
    for (const candidate of m.candidates) files['message-port-order-' + candidate.mode] = {
        schema: 'capsid-message-port-order-v1', complete: true, status: 'complete', mode: candidate.mode,
        runId: m.expected.runId, requestDeadlineMilliseconds: 5000,
        functionalQualification: true, performanceQualification: false,
        workerOsExitNotInferredFromHostExit: true, measuredInputsUnchanged: true,
        inputs: structuredClone(m.expected.messagePortInputs), referenceLock: structuredClone(m.expected.lock),
        binaries: {host: m.expected.host, worker: m.expected.worker, reference: m.expected.lock.binary_sha256,
            node: m.expected.messagePortNode},
        roles: {reference: {httpStatus: 200, public: structuredClone(portSample.public), stderr: '', cleanupRequested: true},
            native: {httpStatus: 200, public: structuredClone(portSample.public), stderr: '',
                shutdown: {code: 0, signal: null, forced: false}}},
    };

    for (const candidate of m.candidates) files[candidate.mode] = candidate;
    const modes = ['reference', ...m.candidates.map(report => report.mode)];
    for (const mode of modes) for (const source of readOrdinaryFetchSamples(m.expected.lock)) {
        const report = structuredClone(source); report.mode = mode; report.runId = m.expected.runId;
        report.binaries = {host: mode === 'reference' ? m.expected.lock.binary_sha256 : m.expected.host,
            worker: mode === 'reference' ? null : m.expected.worker};
        if (mode !== 'reference') for (const value of report.cases) value.shutdown = {code: 0, signal: null, forced: false};
        files['ordinary-fetch-' + report.shard + '-' + mode] = report;
    }
    for (const mode of modes) for (const source of readHttpCodecSamples(m.expected.lock)) {
        const report = structuredClone(source); report.mode = mode; report.runId = m.expected.runId;
        report.binaries = {host: mode === 'reference' ? m.expected.lock.binary_sha256 : m.expected.host,
            worker: mode === 'reference' ? null : m.expected.worker};
        if (mode !== 'reference') for (const value of report.cases) value.shutdown = {code: 0, signal: null, forced: false};
        files['http-codec-' + report.shard + '-' + mode] = report;
    }
    for(const mode of modes) {
        const report=structuredClone(nativeReaderLifecycleSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['native-reader-lifecycle-'+mode]=report;
    }
    // Synthetic product matrices here test validators, never supply live qualification.
    for(const mode of modes) {
        const report=structuredClone(readerScopeSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['reader-scope-'+mode]=report;
    }
    // Synthetic archive controls partition the actual full reference sample.
    // Product qualification still requires fresh reports for every live shard.
    for(const mode of modes) for(const [shard,start,end]of [['shapes',0,80],['ownership',80,130],['transfers',130,230]]) {
        const report=structuredClone(standardReaderSample);report.mode=mode;report.runId=m.expected.runId;report.shard=shard;
        report.cases=report.cases.slice(start,end);report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['standard-readers-'+shard+'-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(readerIntrinsicSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.sourceName=m.expected.readerIntrinsicSourceNames[mode];
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['reader-intrinsics-shared-'+mode]=report;
    }
    for(const mode of modes) for(const family of mode==='reference'?['shared']:['shared','budget']) {
        const report=structuredClone(family==='shared'?asyncSample:budgetSample);
        report.mode=mode;report.runId=m.expected.runId;report.referenceLock=structuredClone(m.expected.lock);
        report.inputs=structuredClone(m.expected.asyncInputs);report.sourceName=m.expected.asyncSourceNames[mode];
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        for(const value of report.cases) {
            if(mode!=='reference')value.shutdown={code:0,signal:null,forced:false};
            for(const event of value.state.state.globalEvents)if(event.filename==='main')event.filename=report.sourceName;
            value.stateText=JSON.stringify(value.state);
            if(value.response)value.response.text=value.stateText;
        }
        files['async-'+family+'-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(crossSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference') for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['cross-context-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(resourceSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['fetch-resources-'+mode]=report;
    }
    // Synthetic healthy candidates exercise the complete gate. The stored
    // reference close-race event is retained only in the reference report.
    for(const mode of modes) {
        const report=structuredClone(upgradeSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases) {
            value.shutdown={code:0,signal:null,forced:false};
            for(const key of ['atClose','observed']) {
                const document=JSON.parse(value[key].text);
                if(document.state.socket)document.state.socket.events=document.state.socket.events.filter(event=>event.type!=='error');
                value[key].text=JSON.stringify(document);
            }
        }
        files['fetch-upgrade-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(lifetimeSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases) {
            value.shutdown={code:0,signal:null,forced:false};
            for(const key of ['beforeAction','afterAction','closedParent','checkpoint','afterRelease','observed','finalState'])if(value[key]) {
                const document=JSON.parse(value[key].text);
                for(const owner of Object.values(document.state.owners??{a:document.state}))if(owner?.socket)owner.socket.events=owner.socket.events.filter(event=>event.type!=='error');
                value[key].text=JSON.stringify(document);
            }
        }
        files['fetch-upgrade-lifetime-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(proxySample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['fetch-proxy-'+mode]=report;
    }
    // Profile/backend copies exercise archive validation only. Native reports
    // for all actual platforms and modes are mandatory in the real gate.
    for(const mode of modes) {
        const report=structuredClone(boundarySample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases){
            value.shutdown={code:0,signal:null,forced:false};
            const document=JSON.parse(value.observed.text);
            if(document.state.socket)document.state.socket.events=document.state.socket.events.filter(event=>event.type!=='error');
            value.observed.text=JSON.stringify(document);
        }
        files['fetch-boundary-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(transitionSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases){
            value.shutdown={code:0,signal:null,forced:false};
            const document=JSON.parse(value.observed.text);
            if(document.state.socket)document.state.socket.events=document.state.socket.events.filter(event=>event.type!=='error');
            value.observed.text=JSON.stringify(document);
        }
        files['fetch-transition-'+mode]=report;
    }
    for(const mode of modes)for(const [shard,start,end] of [['readers',0,100],['constructors',100,200],['reflect-standard',200,306],['views-brands',306,414],['bodies',414,435],['standard-edges',435,471],['standard-close',471,487]]) {
        const report=structuredClone({...byobSample,cases:byobSample.cases.slice(start,end)});report.mode=mode;report.runId=m.expected.runId;report.shard=shard;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['fetch-byob-'+shard+'-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(cryptoSample);report.mode=mode;report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['crypto-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(ownerSample); report.mode=mode; report.runId=m.expected.runId;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference') for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};
        files['owner-lifecycle-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(frameSample);report.mode=mode;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')report.shutdown={code:0,signal:null,forced:false};
        files['frame-boundaries-'+mode]=report;
    }
    for(const mode of modes) {
        const report=structuredClone(interopSample);report.mode=mode;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference')report.shutdown={code:0,signal:null,forced:false};
        files['interop-'+mode]=report;
    }
    for(const mode of modes) {
        // Unit archive candidates are synthetic copies of an actual reference
        // observation; required CTests separately execute every real backend.
        const report=JSON.parse(JSON.stringify(fanoutSample).replaceAll(fanoutSample.runId,m.expected.runId));
        report.mode=mode;
        report.referenceLock=structuredClone(m.expected.lock);
        report.binaries={host:mode==='reference'?m.expected.lock.binary_sha256:m.expected.host,worker:mode==='reference'?null:m.expected.worker};
        if(mode!=='reference') {
            const count=mode==='single-worker'?2:1;
            report.binaries={host:m.expected.host,worker:m.expected.worker};
            report.topology={workers:2,hostProcesses:count,referenceServices:0};
            report.shutdowns=Array.from({length:count},()=>({code:0,signal:null,forced:false}));
            report.backendStderr=Array(count).fill('');
            if(mode!=='single-worker') {
                report.endpoints=report.endpoints.slice(0,1);report.healthy=report.healthy.slice(0,1);
                for(const entry of report.discovery)entry.endpoint=report.endpoints[0];
            }
        }
        files['fanout-'+mode]=report;
    }
    for (const report of [outbound.reference, ...outbound.candidates]) {
        if (platform === 'win32' && report.mode === 'managed') continue;
        const repeated = report.cases.find(c => c.name === 'ws://localhost/badkey:echo');
        report.cases.push(...Array.from({length:140}, () => structuredClone(repeated)));
        Object.assign(report, {caseInvocations:report.cases.length,
            scenarioTypes:new Set(report.cases.map(c=>c.name)).size,
            referenceLock:m.expected.lock, sourceSHA256:m.expected.outboundRunner,
            hostSHA256:report.mode === 'workerd' ? m.expected.lock.binary_sha256 : m.expected.host,
            workerSHA256:m.expected.worker});
        files['outbound-' + (report.mode === 'workerd' ? 'reference' : report.mode)] = report;
    }
    const api = structuredClone(m.candidates[0].results.find(x=>x.fixture === 'api-compat.mjs'));
    api.assertions = 3009;
    const baselineReport = {...structuredClone(m.candidates[0]), schema:'capsid-ws-fixtures-v1',
        complete:false, reference_parity:true, runner_sha256:m.expected.mutationRunner, results:[api]};
    files['mutation-controls'] = {schema:'capsid-workerd-mutations-v1', complete:true,
        runId:m.expected.runId, mode:'single-worker', client:structuredClone(m.reference.client),
        lock:structuredClone(m.expected.lock), fixtures:structuredClone(m.expected.fixtures),
        hostSHA256:m.expected.host, workerSHA256:m.expected.worker,
        sourceRunnerSHA256:m.expected.runner, runnerSHA256:m.expected.mutationRunner,
        controllerSHA256:m.expected.mutationController, contractSHA256:m.expected.mutationContract,
        handlerObserverSHA256:m.expected.handlerErrorObserver,
        fixtureSHA256:m.expected.fixtures.find(x=>x.fixture === 'api-compat.mjs').sha256,
        results:mutationCases.map(([name, diagnostic]) => ({name, exit:diagnostic ? 1 : 0,
            handlerErrorCase:diagnostic?.source.includes(': handler error contract')
                ? diagnostic.source.split(': handler error contract')[0].replaceAll('\\/', '/') : null,
            runnerSHA256:m.expected.mutationRunners[name],
            signal:null, error:null, detected:diagnostic !== null,
            fixtureSHA256:diagnostic ? createHash('sha256').update(name).digest('hex') : apiHash(),
            expectedDiagnostic:diagnostic?.source ?? null,
            stdout:diagnostic ? '' : 'api-compat.mjs: passed (3009 assertions)\n',
            stderr:diagnostic ? `AssertionError: ${diagnostic.source.replaceAll('\\/', '/')}\n` : '',
            ...(diagnostic ? {} : {driverReport:baselineReport})}))};
    function apiHash() { return m.expected.fixtures.find(x=>x.fixture === 'api-compat.mjs').sha256; }
    const names = ['workerd_ws_fixtures', 'workerd_ws_fixtures_static-pool', 'workerd_ws_fixtures_managed',
        'native_message_port_order_single-worker', 'native_message_port_order_static-pool', 'native_message_port_order_managed',
        'workerd_ws_reference', 'websocket_outbound_reference', 'websocket_outbound_single-worker',
        'websocket_outbound_static-pool', 'websocket_outbound_managed', 'workerd_ws_gate_controls',
        'workerd_ws_gate_archive_controls', 'workerd_ws_gate_extension_controls','workerd_ws_gate_support_controls',
        'workerd_ws_mutation_controls','workerd_throw_contract_controls','workerd_handler_error_contract_controls',
        'workerd_owner_contract_controls','websocket_owner_lifecycle_reference',
        'websocket_owner_lifecycle_single-worker','websocket_owner_lifecycle_static-pool','websocket_owner_lifecycle_managed',
        'workerd_async_callback_contract_controls','websocket_async_callbacks_reference',
        'websocket_async_callbacks_single-worker','websocket_async_callbacks_static-pool','websocket_async_callbacks_managed',
        'websocket_async_budget_single-worker','websocket_async_budget_static-pool','websocket_async_budget_managed',
        'workerd_reader_intrinsic_contract_controls','websocket_reader_intrinsics_reference',
        'websocket_reader_intrinsics_single-worker','websocket_reader_intrinsics_static-pool','websocket_reader_intrinsics_managed',
        'workerd_native_reader_lifecycle_contract_controls','websocket_native_reader_lifecycle_reference',
        'workerd_reader_scope_contract_controls', 'websocket_reader_scope_reference',
    'websocket_reader_scope_single-worker', 'websocket_reader_scope_static-pool', 'websocket_reader_scope_managed', 'workerd_standard_reader_contract_controls','websocket_standard_readers_shapes_reference', 'websocket_standard_readers_ownership_reference', 'websocket_standard_readers_transfers_reference',
        'websocket_native_reader_lifecycle_single-worker','websocket_native_reader_lifecycle_static-pool','websocket_native_reader_lifecycle_managed',
        'websocket_standard_readers_shapes_single-worker', 'websocket_standard_readers_ownership_single-worker', 'websocket_standard_readers_transfers_single-worker','websocket_standard_readers_shapes_static-pool', 'websocket_standard_readers_ownership_static-pool', 'websocket_standard_readers_transfers_static-pool','websocket_standard_readers_shapes_managed', 'websocket_standard_readers_ownership_managed', 'websocket_standard_readers_transfers_managed',
        'workerd_cross_context_contract_controls','websocket_cross_context_reference',
        'websocket_cross_context_single-worker','websocket_cross_context_static-pool','websocket_cross_context_managed',
        'workerd_fetch_resource_contract_controls','websocket_fetch_resources_reference',
        'websocket_fetch_resources_single-worker','websocket_fetch_resources_static-pool','websocket_fetch_resources_managed',
        'workerd_ordinary_fetch_contract_controls',
        'websocket_ordinary_fetch_core_reference', 'websocket_ordinary_fetch_core_single-worker', 'websocket_ordinary_fetch_core_static-pool', 'websocket_ordinary_fetch_core_managed',
        'websocket_ordinary_fetch_pipelines_reference', 'websocket_ordinary_fetch_pipelines_single-worker', 'websocket_ordinary_fetch_pipelines_static-pool', 'websocket_ordinary_fetch_pipelines_managed',
        'websocket_ordinary_fetch_gated_reference', 'websocket_ordinary_fetch_gated_single-worker', 'websocket_ordinary_fetch_gated_static-pool', 'websocket_ordinary_fetch_gated_managed',
        'websocket_ordinary_fetch_views_reference', 'websocket_ordinary_fetch_views_single-worker', 'websocket_ordinary_fetch_views_static-pool', 'websocket_ordinary_fetch_views_managed',
        'websocket_ordinary_fetch_bodies_reference', 'websocket_ordinary_fetch_bodies_single-worker', 'websocket_ordinary_fetch_bodies_static-pool', 'websocket_ordinary_fetch_bodies_managed',

        'http_gzip_parser', 'workerd_http_codec_contract_controls',
        'websocket_http_codec_decode_reference', 'websocket_http_codec_decode_single-worker', 'websocket_http_codec_decode_static-pool', 'websocket_http_codec_decode_managed',
        'websocket_http_codec_metadata_reference', 'websocket_http_codec_metadata_single-worker', 'websocket_http_codec_metadata_static-pool', 'websocket_http_codec_metadata_managed',
        'workerd_fetch_upgrade_contract_controls','websocket_fetch_upgrade_reference',
        'websocket_fetch_upgrade_single-worker','websocket_fetch_upgrade_static-pool','websocket_fetch_upgrade_managed',
        'workerd_fetch_upgrade_lifetime_contract_controls','websocket_fetch_upgrade_lifetime_reference',
        'websocket_fetch_upgrade_lifetime_single-worker','websocket_fetch_upgrade_lifetime_static-pool','websocket_fetch_upgrade_lifetime_managed',
        'workerd_fetch_proxy_contract_controls','websocket_fetch_proxy_reference',
        'websocket_fetch_proxy_single-worker','websocket_fetch_proxy_static-pool','websocket_fetch_proxy_managed',
        'workerd_fetch_boundary_contract_controls','websocket_fetch_boundaries_reference',
        'websocket_fetch_boundaries_single-worker','websocket_fetch_boundaries_static-pool','websocket_fetch_boundaries_managed',
        'workerd_fetch_transition_contract_controls','websocket_fetch_transitions_reference',
        'websocket_fetch_transitions_single-worker','websocket_fetch_transitions_static-pool','websocket_fetch_transitions_managed',
        'workerd_fetch_byob_contract_controls',
        'websocket_fetch_byob_readers_reference','websocket_fetch_byob_readers_single-worker','websocket_fetch_byob_readers_static-pool','websocket_fetch_byob_readers_managed',
        'websocket_fetch_byob_constructors_reference','websocket_fetch_byob_constructors_single-worker','websocket_fetch_byob_constructors_static-pool','websocket_fetch_byob_constructors_managed',
        'websocket_fetch_byob_reflect-standard_reference','websocket_fetch_byob_reflect-standard_single-worker','websocket_fetch_byob_reflect-standard_static-pool','websocket_fetch_byob_reflect-standard_managed',
        'websocket_fetch_byob_views-brands_reference','websocket_fetch_byob_views-brands_single-worker','websocket_fetch_byob_views-brands_static-pool','websocket_fetch_byob_views-brands_managed',
        'websocket_fetch_byob_bodies_reference','websocket_fetch_byob_bodies_single-worker','websocket_fetch_byob_bodies_static-pool','websocket_fetch_byob_bodies_managed',
        'websocket_fetch_byob_standard-edges_reference','websocket_fetch_byob_standard-edges_single-worker','websocket_fetch_byob_standard-edges_static-pool','websocket_fetch_byob_standard-edges_managed',
        'websocket_fetch_byob_standard-close_reference','websocket_fetch_byob_standard-close_single-worker','websocket_fetch_byob_standard-close_static-pool','websocket_fetch_byob_standard-close_managed',
        'workerd_crypto_contract_controls','websocket_crypto_reference',
        'websocket_crypto_single-worker','websocket_crypto_static-pool','websocket_crypto_managed',
        'websocket_interop_reference','websocket_interop_single-worker',
        'websocket_interop_static-pool','websocket_interop_managed',
        'workerd_fanout_reference','workerd_fanout_single-worker','workerd_fanout_static-pool','workerd_fanout_managed',
        'websocket_frame_boundaries_reference','websocket_frame_boundaries_single-worker',
        'websocket_frame_boundaries_static-pool','websocket_frame_boundaries_managed'];
    const selectedNames = platform === 'win32' ? names.filter(name => !name.endsWith('_managed')) : names;
    const result = {files, expected:m.expected,
        junit:`<testsuite>${selectedNames.map(name=>`<testcase name="${name}"/>`).join('')}</testsuite>`};
    archiveTemplates.set(platform, result);
    return cloneConstructionTemplate(result);
}

let archiveScratch;
const archiveTemplateText = new Map();
function archiveInputText(name, data) {
    // These are immutable construction inputs for synthetic controls, never
    // live observations or validator results. Compare the entire current
    // model on every call: object identity alone must not reuse stale bytes.
    for (const [platform, template] of archiveTemplates) {
        if (!Object.hasOwn(template.files, name) ||
            !isDeepStrictEqual(data, template.files[name])) continue;
        const key = platform + '/' + name;
        if (!archiveTemplateText.has(key))
            archiveTemplateText.set(key, JSON.stringify(template.files[name]));
        return archiveTemplateText.get(key);
    }
    return JSON.stringify(data);
}
after(() => {
    if (!archiveScratch) return;
    const target = fs.realpathSync(archiveScratch.dir);
    assert.equal(path.dirname(target), archiveScratch.parent, 'cleanup remains in the owned temporary root');
    assert.ok(path.basename(target).startsWith('capsid-gate-archive-'));
    fs.rmSync(target, {recursive:true, force:true});
});
function verifyArchive(m) {
    if (!archiveScratch) {
        const parent = fs.realpathSync(os.tmpdir());
        archiveScratch = {parent, dir:fs.mkdtempSync(path.join(parent,'capsid-gate-archive-')), written:new Map()};
    }
    const {dir, written} = archiveScratch;
    const desired = new Map([['junit.xml', m.junit]]);
    for (const [name,data] of Object.entries(m.files)) {
        assert.match(name,/^[A-Za-z0-9._-]+$/, 'controlled archive filename');
        desired.set(name+'.json',archiveInputText(name, data));
    }
    // Remove omitted reports and restore every changed byte before each
    // validation. Every validator still reads the complete actual archive;
    // stable files avoid repeated Windows create/delete/scan overhead.
    for (const name of written.keys()) if (!desired.has(name)) {
        fs.unlinkSync(path.join(dir,name)); written.delete(name);
    }
    for (const [name,data] of desired) if (written.get(name)!==data) {
        fs.writeFileSync(path.join(dir,name),data); written.set(name,data);
    }
    return validateGateRun(dir,m.expected);
}

test('native asset locks bind the same source profile and distinct official executables', () => {
    const linux = matrix().expected.lock, windows = matrix('win32').expected.lock;
    for (const key of ['source_commit', 'release_tag', 'binary_version_string', 'compatibility_date', 'compatibility_flags', 'client'])
        assert.deepEqual(windows[key], linux[key], 'same reference profile: ' + key);
    assert.equal(referencePlatform(linux), 'linux');
    assert.equal(referencePlatform(windows), 'win32');
    assert.deepEqual(referenceBackendModes(linux), ['single-worker', 'static-pool', 'managed']);
    assert.deepEqual(referenceBackendModes(windows), ['single-worker', 'static-pool']);
    assert.equal(windows.binary_sha256, 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a');
    assert.equal(windows.release_asset_sha256, '8b9377af95e2649bb1d0ea20525ac31769f38c1d85f1b5922a3bbe0856462a82');
    assert.equal(readLock().release_asset, process.platform === 'win32' ? 'workerd-windows-64.gz' : 'workerd-linux-64.gz');
});

test('foreign platform binaries are rejected before execution', () => {
    const foreign = matrix(process.platform === 'win32' ? 'linux' : 'win32').expected.lock;
    assert.throws(() => verifyBinary('not-opened-or-executed', foreign), /locked workerd executable platform/);
});

for (const [name, mutate] of [
    ['missing resource case everywhere', report => report.cases.splice(0, 1)],
    ['unclosed resource everywhere', report => {report.cases.find(value => value.id === 'fetch-close-pending').upstream[0].peerEnded = false;}],
    ['wrong cancellation identity everywhere', report => {
        const value = report.cases.find(entry => entry.id === 'fetch-abort-pre-error');
        const document = JSON.parse(value.observed.text); document.state.failure.sameReason = false; value.observed.text = JSON.stringify(document);
    }],
]) test('required gate rejects ' + name, () => {
    const m = archiveMatrix('win32');
    for (const [key, report] of Object.entries(m.files)) if (key.startsWith('fetch-resources-')) mutate(report);
    assert.throws(() => verifyArchive(m));
});

test('Windows requires 134 exact CTests and both supported backend reports', () => {
    const m = archiveMatrix('win32');
    const expectedNames = ['workerd_ws_fixtures', 'workerd_ws_fixtures_static-pool', 'workerd_ws_reference',
        'native_message_port_order_single-worker', 'native_message_port_order_static-pool',
        'websocket_outbound_reference', 'websocket_outbound_single-worker', 'websocket_outbound_static-pool',
        'workerd_ws_gate_controls', 'workerd_ws_gate_archive_controls', 'workerd_ws_gate_extension_controls', 'workerd_ws_gate_support_controls', 'workerd_ws_mutation_controls', 'workerd_throw_contract_controls',
        'workerd_handler_error_contract_controls',
        'workerd_owner_contract_controls', 'websocket_owner_lifecycle_reference',
        'websocket_owner_lifecycle_single-worker', 'websocket_owner_lifecycle_static-pool',
        'workerd_async_callback_contract_controls', 'websocket_async_callbacks_reference',
        'websocket_async_callbacks_single-worker', 'websocket_async_callbacks_static-pool',
        'websocket_async_budget_single-worker', 'websocket_async_budget_static-pool',
        'workerd_reader_intrinsic_contract_controls', 'websocket_reader_intrinsics_reference',
        'websocket_reader_intrinsics_single-worker', 'websocket_reader_intrinsics_static-pool',
        'workerd_native_reader_lifecycle_contract_controls', 'websocket_native_reader_lifecycle_reference',
        'workerd_reader_scope_contract_controls', 'websocket_reader_scope_reference',
    'websocket_reader_scope_single-worker', 'websocket_reader_scope_static-pool', 'workerd_standard_reader_contract_controls', 'websocket_standard_readers_shapes_reference', 'websocket_standard_readers_ownership_reference', 'websocket_standard_readers_transfers_reference',
        'websocket_native_reader_lifecycle_single-worker', 'websocket_native_reader_lifecycle_static-pool',
        'websocket_standard_readers_shapes_single-worker', 'websocket_standard_readers_ownership_single-worker', 'websocket_standard_readers_transfers_single-worker', 'websocket_standard_readers_shapes_static-pool', 'websocket_standard_readers_ownership_static-pool', 'websocket_standard_readers_transfers_static-pool',
        'workerd_cross_context_contract_controls', 'websocket_cross_context_reference',
        'websocket_cross_context_single-worker', 'websocket_cross_context_static-pool',
        'workerd_fetch_resource_contract_controls', 'websocket_fetch_resources_reference',
        'websocket_fetch_resources_single-worker', 'websocket_fetch_resources_static-pool',
        'workerd_ordinary_fetch_contract_controls',
        'websocket_ordinary_fetch_core_reference', 'websocket_ordinary_fetch_core_single-worker', 'websocket_ordinary_fetch_core_static-pool',
        'websocket_ordinary_fetch_pipelines_reference', 'websocket_ordinary_fetch_pipelines_single-worker', 'websocket_ordinary_fetch_pipelines_static-pool',
        'websocket_ordinary_fetch_gated_reference', 'websocket_ordinary_fetch_gated_single-worker', 'websocket_ordinary_fetch_gated_static-pool',
        'websocket_ordinary_fetch_views_reference', 'websocket_ordinary_fetch_views_single-worker', 'websocket_ordinary_fetch_views_static-pool',
        'websocket_ordinary_fetch_bodies_reference', 'websocket_ordinary_fetch_bodies_single-worker', 'websocket_ordinary_fetch_bodies_static-pool',

        'http_gzip_parser', 'workerd_http_codec_contract_controls',
        'websocket_http_codec_decode_reference', 'websocket_http_codec_decode_single-worker', 'websocket_http_codec_decode_static-pool',
        'websocket_http_codec_metadata_reference', 'websocket_http_codec_metadata_single-worker', 'websocket_http_codec_metadata_static-pool',
        'workerd_fetch_upgrade_contract_controls', 'websocket_fetch_upgrade_reference',
        'websocket_fetch_upgrade_single-worker', 'websocket_fetch_upgrade_static-pool',
        'workerd_fetch_upgrade_lifetime_contract_controls', 'websocket_fetch_upgrade_lifetime_reference',
        'websocket_fetch_upgrade_lifetime_single-worker', 'websocket_fetch_upgrade_lifetime_static-pool',
        'workerd_fetch_proxy_contract_controls', 'websocket_fetch_proxy_reference',
        'websocket_fetch_proxy_single-worker', 'websocket_fetch_proxy_static-pool',
        'workerd_fetch_boundary_contract_controls', 'websocket_fetch_boundaries_reference',
        'websocket_fetch_boundaries_single-worker', 'websocket_fetch_boundaries_static-pool',
        'workerd_fetch_transition_contract_controls', 'websocket_fetch_transitions_reference',
        'websocket_fetch_transitions_single-worker', 'websocket_fetch_transitions_static-pool',
        'workerd_fetch_byob_contract_controls',
        'websocket_fetch_byob_readers_reference', 'websocket_fetch_byob_readers_single-worker', 'websocket_fetch_byob_readers_static-pool',
        'websocket_fetch_byob_constructors_reference', 'websocket_fetch_byob_constructors_single-worker', 'websocket_fetch_byob_constructors_static-pool',
        'websocket_fetch_byob_reflect-standard_reference', 'websocket_fetch_byob_reflect-standard_single-worker', 'websocket_fetch_byob_reflect-standard_static-pool',
        'websocket_fetch_byob_views-brands_reference', 'websocket_fetch_byob_views-brands_single-worker', 'websocket_fetch_byob_views-brands_static-pool',
        'websocket_fetch_byob_bodies_reference', 'websocket_fetch_byob_bodies_single-worker', 'websocket_fetch_byob_bodies_static-pool',
        'websocket_fetch_byob_standard-edges_reference', 'websocket_fetch_byob_standard-edges_single-worker', 'websocket_fetch_byob_standard-edges_static-pool',
        'websocket_fetch_byob_standard-close_reference', 'websocket_fetch_byob_standard-close_single-worker', 'websocket_fetch_byob_standard-close_static-pool',
        'workerd_crypto_contract_controls', 'websocket_crypto_reference',
        'websocket_crypto_single-worker', 'websocket_crypto_static-pool',
        'websocket_interop_reference', 'websocket_interop_single-worker', 'websocket_interop_static-pool',
        'workerd_fanout_reference', 'workerd_fanout_single-worker', 'workerd_fanout_static-pool',
        'websocket_frame_boundaries_reference', 'websocket_frame_boundaries_single-worker',
        'websocket_frame_boundaries_static-pool'];
    assert.deepEqual(requiredTestsFor(m.expected.lock).sort(), expectedNames.sort());
    const report = verifyArchive(m);
    assert.equal(report.tests.length, 134);
    assert.deepEqual(report.modes, ['single-worker', 'static-pool']);
    for (const field of ['interop', 'fanout', 'frames', 'owners', 'crossContext', 'fetchResources']) assert.equal(report[field].backends, 3);
    assert.equal(report.owners.casesPerBackend, 17);
    assert.deepEqual(report.asyncCallbacks,{complete:true,casesPerBackend:24,backends:3,budgetCasesPerBackend:5,budgetBackends:2});
    assert.deepEqual(report.readerIntrinsics,{complete:true,casesPerBackend:40,backends:2});
    assert.deepEqual(report.nativeReaderLifecycle,{complete:true,casesPerBackend:100,backends:2});
    assert.deepEqual(report.readerScope,{complete:true,casesPerBackend:160,backends:2});
    assert.deepEqual(report.standardReaders,{complete:true,casesPerBackend:230,shardsPerBackend:3,backends:2});
    assert.deepEqual(report.crossContext,{complete:true,casesPerBackend:26,backends:3});
    assert.deepEqual(report.fetchResources,{complete:true,casesPerBackend:32,backends:3});
    assert.deepEqual(report.ordinaryFetch, {complete: true, casesPerBackend: 106, backends: 3,
        shards: [{shard: 'core', cases: 30}, {shard: 'pipelines', cases: 14}, {shard: 'gated', cases: 8}, {shard: 'views', cases: 30}, {shard: 'bodies', cases: 24}]});
    assert.deepEqual(report.httpCodec, {complete: true, casesPerBackend: 65, backends: 3,
        shards: [{shard: 'decode', cases: 34}, {shard: 'metadata', cases: 31}]});
    assert.deepEqual(report.fetchUpgrade,{complete:true,casesPerBackend:13,backends:2});
    assert.deepEqual(report.fetchUpgradeLifetime,{complete:true,casesPerBackend:19,backends:2});
    assert.deepEqual(report.fetchProxy,{complete:true,casesPerBackend:22,backends:2});
    assert.deepEqual(report.fetchBoundaries,{complete:true,casesPerBackend:22,backends:2});
    assert.deepEqual(report.fetchTransitions,{complete:true,casesPerBackend:46,backends:2});
    assert.deepEqual(report.fetchBYOB,{complete:true,casesPerBackend:487,backends:2,shardsPerBackend:7});
    assert.deepEqual(report.crypto,{complete:true,casesPerBackend:42,backends:2});
    assert.equal(requiredTestsFor(matrix().expected.lock).length, 171, 'Linux keeps every managed backend test');
});

for (const [name, mutate] of [
    ['missing current upgrade report', m => {delete m.files['fetch-upgrade-managed'];}],
    ['stale upgrade input', m => {m.files['fetch-upgrade-single-worker'].inputs.contract='a'.repeat(64);}],
    ['stale upgrade binary', m => {m.files['fetch-upgrade-static-pool'].binaries.worker='a'.repeat(64);}],
    ['incomplete upgrade collection', m => {m.files['fetch-upgrade-reference'].complete=false;}],
    ['same upgrade case missing everywhere', m => {
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-upgrade-')&&!key.startsWith('fetch-upgrade-lifetime-'))report.cases.pop();
    }],
    ['same wrong HTTP status everywhere', m => {
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-upgrade-')&&!key.startsWith('fetch-upgrade-lifetime-'))for(const field of ['observed','atClose']) {
            const document=JSON.parse(report.cases[0][field].text);document.state.result.status=400;report.cases[0][field].text=JSON.stringify(document);
        }
    }],
    ['same missing upstream Close everywhere', m => {
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-upgrade-')&&!key.startsWith('fetch-upgrade-lifetime-'))report.cases[0].upstream[0].frames.pop();
    }],
]) test('whole gate rejects '+name, () => {
    const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));
});

for (const [name, mutate, diagnostic] of [
    ['missing crypto report', m => {delete m.files['crypto-managed'];}],
    ['incomplete crypto report', m => {m.files['crypto-reference'].complete=false;}, /complete crypto collection/],
    ['stale crypto binary', m => {m.files['crypto-single-worker'].binaries.worker='a'.repeat(64);}, /actual current binaries/],
    ['stale crypto input', m => {m.files['crypto-static-pool'].inputs.fixture='a'.repeat(64);}, /current exact crypto inputs/],
    ['same crypto case omitted everywhere', m => {
        for (const mode of ['reference','single-worker','static-pool','managed']) m.files['crypto-'+mode].cases.pop();
    }, /complete ordered crypto inventory/],
    ['same crypto result corrupted everywhere', m => {
        for (const mode of ['reference','single-worker','static-pool','managed']) {
            const value=m.files['crypto-'+mode].cases[0];
            const document=JSON.parse(value.observed.text);document.state.result=['wrong'];
            value.observed.text=JSON.stringify(document);
        }
    }, /complete same-realm state/],
    ['missing shared async report', m => {delete m.files['async-shared-managed'];}],
    ['missing budget async report', m => {delete m.files['async-budget-managed'];}],
    ['incomplete async report', m => {m.files['async-shared-reference'].complete=false;}, /complete async callback run/],
    ['stale async run', m => {m.files['async-shared-single-worker'].runId='old';}, /current async callback run/],
    ['different async fixture', m => {m.files['async-shared-static-pool'].inputs.fixture='0'.repeat(64);}, /input provenance/],
    ['same async case omitted everywhere', m => {
        for(const [name,report] of Object.entries(m.files))if(name.startsWith('async-shared-'))report.cases.pop();
    }, /complete ordered async callback inventory/],
    ['same async event corruption everywhere', m => {
        for(const [name,report] of Object.entries(m.files))if(name.startsWith('async-shared-')) {
            report.cases[4].state.state.globalEvents[0].cancelable=true;
            report.cases[4].stateText=JSON.stringify(report.cases[4].state);
        }
    }, /complete HTTP state/],
    ['async budget is weakened', m => {m.files['async-budget-managed'].requestTimeoutMs=10000;}, /fixed async callback execution budget/],
    ['same budget loop omitted everywhere', m => {
        for(const [name,report] of Object.entries(m.files))if(name.startsWith('async-budget-'))report.cases.shift();
    }, /complete ordered async callback inventory/],
    ['shared run replaced by a budget alias', m => {m.files['async-shared-single-worker']=m.files['async-budget-single-worker'];}, /async callback report family/],
    ['async process forced to stop', m => {m.files['async-shared-static-pool'].cases[0].shutdown.forced=true;}, /cooperative shutdown/],
]) test('required gate rejects '+name, () => {
    const m=archiveMatrix(); mutate(m); assert.throws(()=>verifyArchive(m), diagnostic);
});
for (const [name, mutate, diagnostic] of [
    ['missing owner report', m => {delete m.files['owner-lifecycle-managed'];}],
    ['incomplete owner report', m => {m.files['owner-lifecycle-reference'].complete=false;}, /complete connection owner run/],
    ['stale owner run', m => {m.files['owner-lifecycle-single-worker'].runId='old-run';}, /current connection owner run/],
    ['different owner fixture', m => {m.files['owner-lifecycle-static-pool'].inputs.fixture='0'.repeat(64);}, /input provenance/],
    ['same owner case omitted everywhere', m => {
        for(const [name, report] of Object.entries(m.files))if(name.startsWith('owner-lifecycle-'))report.cases.pop();
    }, /complete ordered connection owner inventory/],
    ['same owner timer corruption everywhere', m => {
        for(const [name, report] of Object.entries(m.files))if(name.startsWith('owner-lifecycle-')) {
            report.cases[0].state.state.timerFired=0; report.cases[0].stateText=JSON.stringify(report.cases[0].state);
        }
    }, /actual HTTP state/],
    ['owner process forced to stop', m => {m.files['owner-lifecycle-static-pool'].cases[0].shutdown.forced=true;}, /cooperative shutdown/],
]) test('required gate rejects '+name, () => {
    const m=archiveMatrix(); mutate(m); assert.throws(()=>verifyArchive(m), diagnostic);
});

for (const name of ['static-pool', 'outbound-static-pool', 'interop-static-pool', 'fanout-static-pool', 'frame-boundaries-static-pool', 'owner-lifecycle-static-pool', 'async-shared-static-pool', 'async-budget-static-pool', 'cross-context-static-pool'])
    test(`Windows rejects missing actual backend report ${name}`, () => {
        const m = archiveMatrix('win32');delete m.files[name];
        assert.throws(() => verifyArchive(m));
    });

test('Windows rejects an invented managed backend in the required test inventory', () => {
    const m = archiveMatrix('win32');m.junit=m.junit.replace('</testsuite>','<testcase name="workerd_ws_fixtures_managed"/></testsuite>');
    assert.throws(() => verifyArchive(m), /required platform CTests actually executed/);
});

test('Windows cannot substitute the Linux reference executable digest', () => {
    const m = archiveMatrix('win32');m.files.reference.binary_sha256=matrix().expected.lock.binary_sha256;
    assert.throws(() => verifyArchive(m), /reference binary identity/);
});

test('complete archived gate with all mutation controls passes', () => {
    assert.equal(verifyArchive(archiveMatrix()).complete,true);
});

for (const [name, mutate] of [
    ['missing backend', m=>{delete m.files['frame-boundaries-managed'];}],
    ['incomplete report', m=>{m.files['frame-boundaries-reference'].complete=false;}],
    ['stale run', m=>{m.files['frame-boundaries-single-worker'].runId='previous-run';}],
    ['changed driver', m=>{m.files['frame-boundaries-static-pool'].inputs.runner='0'.repeat(64);}],
    ['missing long frames on every backend', m=>{
        for(const mode of ['reference','single-worker','static-pool','managed'])
            m.files['frame-boundaries-'+mode].observations=m.files['frame-boundaries-'+mode].observations.filter(x=>x.size<65536);
    }],
    ['identical corrupt payloads on every backend', m=>{
        for(const mode of ['reference','single-worker','static-pool','managed'])
            m.files['frame-boundaries-'+mode].observations[0].sha256='0'.repeat(64);
    }],
    ['wrong binary', m=>{m.files['frame-boundaries-static-pool'].binaries.worker='0'.repeat(64);}],
    ['abnormal shutdown', m=>{m.files['frame-boundaries-single-worker'].shutdown.code=1;}],
]) test(`archived gate rejects raw-frame ${name}`, () => {
    const m=archiveMatrix();mutate(m);
    assert.throws(()=>verifyArchive(m));
});

for (const [name, mutate] of [
    ['missing multiworker fanout backend', m=>{delete m.files['fanout-managed'];}],
    ['fanout stale run', m=>{m.files['fanout-reference'].runId='old';}],
    ['fanout omits a real Worker', m=>{m.files['fanout-managed'].discovery.pop();}],
    ['fanout inputs changed', m=>{m.files['fanout-static-pool'].inputs.fixture='0'.repeat(64);}],
    ['fanout errors weakened on every backend', m=>{
        for(const name of ['reference','single-worker','static-pool','managed'])
            m.files['fanout-'+name].probes[0].response.attempts[0].errorName='TypeError';
    }],
    ['missing independent-client backend', m=>{delete m.files['interop-managed'];}],
    ['independent-client incomplete inventory', m=>{m.files['interop-reference'].cases.pop();}],
    ['independent-client stale run', m=>{m.files['interop-single-worker'].runId='old';}],
    ['independent-client wrong Python implementation', m=>{m.files['interop-managed'].clients.python.sourceSHA256='0'.repeat(64);}],
    ['missing report', m=>{delete m.files['mutation-controls'];}],
    ['null report', m=>{m.files['mutation-controls']=null;}],
    ['empty report', m=>{m.files['mutation-controls']={};}],
    ['incomplete report', m=>{m.files['mutation-controls'].complete=false;}],
    ['wrong schema', m=>{m.files['mutation-controls'].schema='unknown';}],
    ['wrong mode', m=>{m.files['mutation-controls'].mode='managed';}],
    ['stale run', m=>{m.files['mutation-controls'].runId='previous-run';}],
    ...['hostSHA256','workerSHA256','sourceRunnerSHA256','runnerSHA256','controllerSHA256',
        'contractSHA256','fixtureSHA256','handlerObserverSHA256'].map(key=>[key,m=>{m.files['mutation-controls'][key]='0'.repeat(64);}]),
    ['missing expected fault program',m=>{delete m.expected.mutationRunners['wire-code'];}],
    ['changed fault program',m=>{m.files['mutation-controls'].results[1].runnerSHA256='0'.repeat(64);}],
    ['wrong handler target',m=>{m.files['mutation-controls'].results[5].handlerErrorCase='c17/TypeError';}],
    ['ordinary fault claiming handler target',m=>{m.files['mutation-controls'].results[1].handlerErrorCase='c17/Error';}],
    ['baseline omits a new case despite a matching counter',m=>{
        delete m.files['mutation-controls'].results[0].driverReport.results[0].observation.handlerErrors['c17/dom-exception'];
    }],
    ['client', m=>{m.files['mutation-controls'].client.node='v0.0.0';}],
    ['reference profile', m=>{m.files['mutation-controls'].lock.compatibility_flags=['different'];}],
    ['fixture inventory', m=>{m.files['mutation-controls'].fixtures.pop();}],
    ['missing fault', m=>{m.files['mutation-controls'].results.pop();}],
    ['duplicate fault', m=>{m.files['mutation-controls'].results[2]=m.files['mutation-controls'].results[1];}],
    ['extra fault', m=>{m.files['mutation-controls'].results.push(m.files['mutation-controls'].results[1]);}],
    ['baseline not first', m=>{m.files['mutation-controls'].results.reverse();}],
    ['baseline failed', m=>{m.files['mutation-controls'].results[0].exit=1;}],
    ['baseline report missing', m=>{delete m.files['mutation-controls'].results[0].driverReport;}],
    ['baseline falsely claims full fixtures', m=>{m.files['mutation-controls'].results[0].driverReport.complete=true;}],
    ['baseline schema', m=>{m.files['mutation-controls'].results[0].driverReport.schema='unknown';}],
    ['baseline stale field difference', m=>{m.files['mutation-controls'].results[0].driverReport.reference_parity=false;}],
    ['baseline zero assertions', m=>{m.files['mutation-controls'].results[0].driverReport.results[0].assertions=0;}],
    ['baseline fewer assertions', m=>{m.files['mutation-controls'].results[0].driverReport.results[0].assertions=647;}],
    ['baseline observation changed', m=>{m.files['mutation-controls'].results[0].driverReport.results[0].observation.received='other';}],
    ['baseline forced shutdown', m=>{m.files['mutation-controls'].results[0].driverReport.results[0].shutdown.forced=true;}],
    ['baseline different binary', m=>{m.files['mutation-controls'].results[0].driverReport.worker_sha256='0'.repeat(64);}],
    ['undetected fault', m=>{m.files['mutation-controls'].results[1].detected=false;}],
    ['fault exited successfully', m=>{m.files['mutation-controls'].results[1].exit=0;}],
    ['signal termination', m=>{m.files['mutation-controls'].results[1].signal='SIGTERM';}],
    ['spawn error', m=>{m.files['mutation-controls'].results[1].error={code:'ETIMEDOUT'};}],
    ['missing completion evidence', m=>{delete m.files['mutation-controls'].results[1].signal;}],
    ['diagnostic weakened', m=>{m.files['mutation-controls'].results[1].expectedDiagnostic='.*';}],
    ['wrong assertion', m=>{m.files['mutation-controls'].results[1].stderr='AssertionError: unrelated';}],
    ['no changed fixture', m=>{m.files['mutation-controls'].results[1].fixtureSHA256=m.files['mutation-controls'].fixtureSHA256;}],
    ['repeated mutation source', m=>{m.files['mutation-controls'].results[2].fixtureSHA256=m.files['mutation-controls'].results[1].fixtureSHA256;}],
]) test(`archived gate rejects mutation ${name}`, () => {
    const m=archiveMatrix(); mutate(m);
    assert.throws(()=>verifyArchive(m));
});

for (const [name, mutate] of [
    ['missing cross-context report', m => {delete m.files['cross-context-managed'];}],
    ['partial cross-context report', m => {m.files['cross-context-reference'].complete=false;}],
    ['stale cross-context run', m => {m.files['cross-context-single-worker'].runId='old-run';}],
    ['different cross-context fixture', m => {m.files['cross-context-static-pool'].inputs.fixture='0'.repeat(64);}],
    ['all cross-context reports omit a case', m => {
        for(const [name,report] of Object.entries(m.files))if(name.startsWith('cross-context-'))report.cases.pop();
    }],
    ['cross-context reports agree on wrong ownership', m => {
        for(const [name,report] of Object.entries(m.files))if(name.startsWith('cross-context-')) {
            const value=report.cases[0],state=JSON.parse(value.state.text);state.trace.push('wrong-owner');value.state.text=JSON.stringify(state);
        }
    }],
    ['cross-context raw bytes disagree with frames', m => {m.files['cross-context-managed'].cases[0].wireb.rawTail='';}],
    ['cross-context completion is local disposal', m => {m.files['cross-context-static-pool'].cases[0].wirea.completion='client-disposed';}],
]) test('gate rejects '+name,()=>assert.throws(()=>{const m=archiveMatrix();mutate(m);verifyArchive(m);}));

for(const [name,mutate] of [
    ['missing lifetime report',m=>{delete m.files['fetch-upgrade-lifetime-managed'];}],
    ['stale lifetime input',m=>{m.files['fetch-upgrade-lifetime-single-worker'].inputs.contract='a'.repeat(64);}],
    ['stale lifetime binary',m=>{m.files['fetch-upgrade-lifetime-static-pool'].binaries.worker='a'.repeat(64);}],
    ['incomplete lifetime collection',m=>{m.files['fetch-upgrade-lifetime-reference'].complete=false;}],
    ['all lifetime reports omit the same case',m=>{for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-upgrade-lifetime-'))report.cases.pop();}],
    ['all lifetime reports prematurely cancel shared origin',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-upgrade-lifetime-')) {
            const value=report.cases.find(value=>value.kind==='same-origin-parent-pending');value.checkpointUpstream[1].peerEnded=true;value.checkpointUpstream[1].events.push('peer-ended');
        }
    }],
    ['all lifetime reports stop the original timer',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-upgrade-lifetime-')) {
            const value=report.cases.find(value=>value.kind==='same-origin-parent-complete'),document=JSON.parse(value.checkpoint.text);
            document.state.owners.b.timerTicks=JSON.parse(value.closedParent.text).state.owners.b.timerTicks;value.checkpoint.text=JSON.stringify(document);
        }
    }],
    ['all lifetime reports retain a late callback',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-upgrade-lifetime-')) {
            const value=report.cases[0],document=JSON.parse(value.finalState.text);document.state.owners.a.trace.push('late');value.finalState.text=JSON.stringify(document);
        }
    }],
])test('whole gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for(const platform of ['linux','win32'])test(platform+' cached templates isolate every negative control',()=>{
    const first=matrix(platform),second=matrix(platform);
    first.reference.complete=false;first.expected.fixtures.pop();
    assert.equal(second.reference.complete,true);assert.equal(second.expected.fixtures.length,15);
    assert.equal(compareReports(second.reference,second.candidates,second.expected).complete,true);
    const damaged=archiveMatrix(platform),fresh=archiveMatrix(platform);
    damaged.files['fetch-upgrade-lifetime-reference'].cases[0].terminalAfterParent=false;
    damaged.expected.fetchUpgradeLifetimeInputs.contract='invalid';
    assert.equal(fresh.files['fetch-upgrade-lifetime-reference'].cases[0].terminalAfterParent,true);
    assert.match(fresh.expected.fetchUpgradeLifetimeInputs.contract,/^[a-f0-9]{64}$/);
    assert.deepEqual(archiveMatrix(platform),fresh);
});

test('archive scratch removes omitted reports and restores every failed mutation',()=>{
    const good=archiveMatrix('win32');assert.equal(verifyArchive(good).complete,true);
    const omitted=archiveMatrix('win32');delete omitted.files['fetch-upgrade-lifetime-single-worker'];
    assert.throws(()=>verifyArchive(omitted));
    assert.equal(fs.existsSync(path.join(archiveScratch.dir,'fetch-upgrade-lifetime-single-worker.json')),false);
    const corrupted=archiveMatrix('win32');corrupted.files['fetch-upgrade-lifetime-single-worker'].binaries.worker='invalid';
    assert.throws(()=>verifyArchive(corrupted));
    assert.equal(verifyArchive(archiveMatrix('win32')).complete,true);
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(archiveScratch.dir,'fetch-upgrade-lifetime-single-worker.json'),'utf8')),good.files['fetch-upgrade-lifetime-single-worker']);
});

for(const [name,mutate] of [
    ['missing proxy report',m=>{delete m.files['fetch-proxy-managed'];}],
    ['stale proxy source',m=>{m.files['fetch-proxy-reference'].inputs.fixture='0'.repeat(64);}],
    ['stale proxy binary',m=>{m.files['fetch-proxy-static-pool'].binaries.worker='0'.repeat(64);}],
    ['incomplete proxy report',m=>{m.files['fetch-proxy-reference'].complete=false;}],
    ['same proxy case omitted everywhere',m=>{for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-proxy-'))report.cases.pop();}],
    ['same proxy source timer incorrectly retained everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-proxy-')){
            const value=report.cases.find(value=>value.kind==='source-lifetime'),document=JSON.parse(value.checkpoint.text);
            document.state.timerFired=1;value.checkpoint.text=JSON.stringify(document);
        }
    }],
    ['same proxy body remains open until service shutdown everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-proxy-')){
            const item=report.cases.find(value=>value.kind==='source-http').checkpointUpstream[1];
            item.peerEnded=false;item.peerReset=false;item.events=['request','held-http'];
        }
    }],
    ['same old cross-request accept error everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('fetch-proxy-')){
            const value=report.cases.find(value=>value.kind==='foreign-methods'),document=JSON.parse(value.foreign.text);
            document.state.foreign.accept.error.name='TypeError';value.foreign.text=JSON.stringify(document);
        }
    }],
])test('full archived gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for(const [name,mutate] of [
    ['missing transition report',m=>{delete m.files['fetch-transition-managed'];}],
    ['stale transition input',m=>{m.files['fetch-transition-reference'].inputs.contract='0'.repeat(64);}],
    ['stale transition executable',m=>{m.files['fetch-transition-static-pool'].binaries.worker='0'.repeat(64);}],
    ['incomplete transition report',m=>{m.files['fetch-transition-reference'].complete=false;}],
    ['same transition case omitted everywhere',m=>{for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-transition-'))r.cases.pop();}],
    ['same native read cancellation fulfills everywhere',m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-transition-')){
            const v=r.cases.find(v=>v.kind==='tls-body-reader-probe-object'),d=JSON.parse(v.observed.text);d.state.pending={value:{done:true,bytes:0}};v.observed.text=JSON.stringify(d);
        }
    }],
    ['same continuation runs after final owner Close everywhere',m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-transition-')){
            const v=r.cases.find(v=>v.kind==='http-created-promise-after-close'),d=JSON.parse(v.observed.text);d.state.future=true;v.observed.text=JSON.stringify(d);
        }
    }],
    ['same fake ClientHello phase everywhere',m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-transition-')){
            const v=r.cases.find(v=>v.kind==='tls-handshake-abort');v.upstreamBeforeStop.records[0].receivedChunks=[];v.upstreamAfterStop=structuredClone(v.upstreamBeforeStop);
        }
    }],
])test('full archived gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for(const [name,mutate] of [
    ['missing boundary report',m=>{delete m.files['fetch-boundary-managed'];}],
    ['stale boundary source',m=>{m.files['fetch-boundary-reference'].inputs.contract='0'.repeat(64);}],
    ['stale boundary executable',m=>{m.files['fetch-boundary-static-pool'].binaries.worker='0'.repeat(64);}],
    ['incomplete boundary report',m=>{m.files['fetch-boundary-reference'].complete=false;}],
    ['same boundary omission everywhere',m=>{for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-boundary-'))r.cases.pop();}],
    ['same old body error everywhere',m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-boundary-')){
            const v=r.cases.find(v=>v.kind==='body-truncated'),d=JSON.parse(v.observed.text);d.state.failure.message='HTTP response body failed';v.observed.text=JSON.stringify(d);
        }
    }],
    ['same source held until shutdown everywhere',m=>{
        for(const [key,r] of Object.entries(m.files))if(key.startsWith('fetch-boundary-'))r.cases.find(v=>v.kind==='tls-upgrade').allSourcesEndedBeforeStop=false;
    }],
])test('full archived gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for(const [name,mutate] of [
    ['missing reader intrinsic report',m=>{delete m.files['reader-intrinsics-shared-managed'];}],
    ['missing reader intrinsic reference',m=>{delete m.files['reader-intrinsics-shared-reference'];}],
    ['stale reader intrinsic input',m=>{m.files['reader-intrinsics-shared-reference'].inputs.fixture='0'.repeat(64);}],
    ['stale reader intrinsic binary',m=>{m.files['reader-intrinsics-shared-static-pool'].binaries.worker='0'.repeat(64);}],
    ['incomplete reader intrinsic report',m=>{m.files['reader-intrinsics-shared-reference'].complete=false;}],
    ['same reader intrinsic case omitted everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('reader-intrinsics-'))report.cases.pop();
    }],
    ['same reader intrinsic worker exit everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('reader-intrinsics-'))report.cases[0].stateStatus=503;
    }],
    ['same reader intrinsic forced cleanup everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('reader-intrinsics-'))report.cases[0].shutdown.forced=true;
    }],
    ...[
        ['internal prototype call',state=>state.traps.push('WeakMap.get')],
        ['second listener lost',state=>state.trace.splice(state.trace.indexOf('second-listener'),1)],
        ['wrong scheduling',state=>state.trace.reverse()],
        ['input not transferred',state=>{state.inputAfter=32;}],
        ['backing changed',state=>{state.read.backing[0]=0;}],
        ['closed identity changed',state=>{state.closedDuringStable=false;}],
        ['lock retained',state=>{state.locked=true;}],
    ].map(([name,mutate])=>['same reader intrinsic '+name+' everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('reader-intrinsics-')){
            const row=report.cases[0];mutate(row.state.state);row.stateText=JSON.stringify(row.state);
        }
    }]),
    ...['workerd_reader_intrinsic_contract_controls','websocket_reader_intrinsics_reference',
        'websocket_reader_intrinsics_single-worker','websocket_reader_intrinsics_static-pool','websocket_reader_intrinsics_managed']
        .map(name=>['missing reader intrinsic CTest '+name,m=>{m.junit=m.junit.replace('<testcase name="'+name+'"/>','');}]),
])test('full archived gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for(const [name,mutate] of [
    ['missing native reader lifecycle report',m=>{delete m.files['native-reader-lifecycle-managed'];}],
    ['stale native reader lifecycle input',m=>{m.files['native-reader-lifecycle-reference'].inputs.fixture='0'.repeat(64);}],
    ['stale native reader lifecycle binary',m=>{m.files['native-reader-lifecycle-static-pool'].binaries.worker='0'.repeat(64);}],
    ['incomplete native reader lifecycle report',m=>{m.files['native-reader-lifecycle-reference'].complete=false;}],
    ['same native reader lifecycle case omitted everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('native-reader-lifecycle-'))report.cases.pop();
    }],
    ['same pending native release accepted everywhere',m=>{
        for(const [key,report] of Object.entries(m.files))if(key.startsWith('native-reader-lifecycle-')){
            const row=report.cases.find(row=>row.kind.endsWith('|release-pending')),document=JSON.parse(row.observed.text);
            document.state.release={returnedUndefined:true};row.observed.text=JSON.stringify(document);
        }
    }],
])test('full archived gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for(const [name,mutate] of [
    ['missing standard reader report',m=>{delete m.files['standard-readers-shapes-managed'];}],
    ['stale standard reader input',m=>{m.files['standard-readers-shapes-reference'].inputs.stateModel='0'.repeat(64);}],
    ['stale standard reader binary',m=>{m.files['standard-readers-shapes-single-worker'].binaries.worker='0'.repeat(64);}],
    ['incomplete standard reader report',m=>{m.files['standard-readers-shapes-reference'].complete=false;}],
    ['same standard reader case omitted everywhere',m=>{
        for(const [key,report]of Object.entries(m.files))if(key.startsWith('standard-readers-'))report.cases.pop();
    }],
    ['same default close buffer identity wrong everywhere',m=>{
        for(const [key,report]of Object.entries(m.files))if(key.startsWith('standard-readers-ownership-')){
            const row=report.cases.find(row=>row.kind.endsWith('|buffer_close_none')),document=JSON.parse(row.observed.text);
            document.state.sameBuffer=true;row.observed.text=JSON.stringify(document);
        }
    }],
    ['same ordered shape run count wrong everywhere',m=>{
        for(const [key,report]of Object.entries(m.files))if(key.startsWith('standard-readers-shapes-')){
            const row=report.cases[0],document=JSON.parse(row.observed.text);document.state.shapes[0].count++;
            row.observed.text=JSON.stringify(document);
        }
    }],
    ['standard byte source performs unexpected I/O',m=>{m.files['standard-readers-shapes-managed'].cases[0].upstreamBeforeStop.tcpRecords.push({closed:true});}],
])test('full archived gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for(const [name,mutate]of [
    ['missing reader scope report',m=>{delete m.files['reader-scope-managed'];}],
    ['stale reader scope input',m=>{m.files['reader-scope-reference'].inputs.stateModel='0'.repeat(64);}],
    ['stale reader scope binary',m=>{m.files['reader-scope-single-worker'].binaries.worker='0'.repeat(64);}],
    ['incomplete reader scope report',m=>{m.files['reader-scope-reference'].complete=false;}],
    ['same reader scope case omitted everywhere',m=>{for(const [key,report]of Object.entries(m.files))if(key.startsWith('reader-scope-'))report.cases.pop();}],
    ['same foreign operation silently accepted everywhere',m=>{for(const [key,report]of Object.entries(m.files))if(key.startsWith('reader-scope-')){const row=report.cases[0],document=JSON.parse(row.observed.text);document.state.actorRead.result={returnedUndefined:true};row.observed.text=JSON.stringify(document);}}],
    ['same retired observer runs everywhere',m=>{for(const [key,report]of Object.entries(m.files))if(key.startsWith('reader-scope-')){const row=report.cases.find(row=>row.kind.endsWith('|foreign_closed_unread')),document=JSON.parse(row.observed.text);document.state.actorClosed.settled=true;row.observed.text=JSON.stringify(document);}}],
    ['foreign actor changed worker',m=>{const row=m.files['reader-scope-static-pool'].cases[0],document=JSON.parse(row.actor.text);document.realm='11111111-1111-4111-8111-111111111111';row.actor.text=JSON.stringify(document);}],
    ['reader scope missing readiness message',m=>{m.files['reader-scope-single-worker'].cases[0].wire.frames.splice(1,1);}],
    ['reader scope source still open',m=>{m.files['reader-scope-managed'].cases[0].allSourcesEndedBeforeStop=false;}],
])test('full archived gate rejects '+name,()=>{const m=archiveMatrix();mutate(m);assert.throws(()=>verifyArchive(m));});

for (const [name, mutate] of [
    ['missing ordinary fetch shard', m => {delete m.files['ordinary-fetch-views-managed'];}],
    ['stale ordinary fetch input', m => {m.files['ordinary-fetch-core-reference'].inputs.contract = '0'.repeat(64);}],
    ['stale ordinary fetch binary', m => {m.files['ordinary-fetch-gated-single-worker'].binaries.worker = '0'.repeat(64);}],
    ['incomplete ordinary fetch shard', m => {m.files['ordinary-fetch-pipelines-reference'].complete = false;}],
    ['same ordinary fetch case omitted everywhere', m => {
        for (const [key, report] of Object.entries(m.files)) if (key.startsWith('ordinary-fetch-views-')) report.cases.pop();
    }],
    ['same ordinary fetch EOF backing wrong everywhere', m => {
        for (const [key, report] of Object.entries(m.files)) if (key.startsWith('ordinary-fetch-views-')) {
            const value = report.cases[0], document = JSON.parse(value.observed.text);
            document.state.rows[0].reads.at(-1).backing[0] = 0;
            value.observed.text = value.finalState.text = JSON.stringify(document);
        }
    }],
    ['ordinary fetch source still alive before host stop', m => {m.files['ordinary-fetch-pipelines-static-pool'].cases[0].upstream[0].peerEnded = false;}],
    ['ordinary fetch cancel completed by fixture', m => {
        const value = m.files['ordinary-fetch-gated-managed'].cases.find(value => value.id === 'ordinary-gated-default-cancel');
        value.upstream[0].events = value.upstream[0].events.filter(event => event !== 'server-half-close');
        value.upstream[0].events.push('server-complete');
    }],
]) test('full archived gate rejects ' + name, () => {const m = archiveMatrix(); mutate(m); assert.throws(() => verifyArchive(m));});


for (const [name, mutate] of [
    ['missing HTTP codec shard', m => {delete m.files['http-codec-metadata-managed'];}],
    ['stale HTTP codec input', m => {m.files['http-codec-decode-reference'].inputs.contract = '0'.repeat(64);}],
    ['stale HTTP codec binary', m => {m.files['http-codec-decode-single-worker'].binaries.worker = '0'.repeat(64);}],
    ['incomplete HTTP codec shard', m => {m.files['http-codec-decode-reference'].complete = false;}],
    ['same HTTP codec case omitted everywhere', m => {
        for (const [key, report] of Object.entries(m.files)) if (key.startsWith('http-codec-decode-')) report.cases.pop();
    }],
    ['same HTTP codec EOF backing wrong everywhere', m => {
        for (const [key, report] of Object.entries(m.files)) if (key.startsWith('http-codec-metadata-')) {
            const value = report.cases.find(value => value.id === 'codec-supp-raw-byob-65536'), document = JSON.parse(value.observed.text);
            document.state.reads.at(-1).backing[0] = 0;
            value.observed.text = value.finalState.text = JSON.stringify(document);
        }
    }],
    ['HTTP codec source alive before host stop', m => {m.files['http-codec-decode-static-pool'].cases[0].upstream[0].peerEnded = false;}],
    ['same HTTP gzip CRC ignored everywhere', m => {
        for (const [key, report] of Object.entries(m.files)) if (key.startsWith('http-codec-decode-')) {
            const value = report.cases.find(value => value.id === 'codec-edge-crc-97'), document = JSON.parse(value.observed.text);
            document.state.failure = null; document.state.closed = {fulfilled: true};
            value.observed.text = value.finalState.text = JSON.stringify(document);
        }
    }],
]) test('full archived gate rejects ' + name, () => {const m = archiveMatrix(); mutate(m); assert.throws(() => verifyArchive(m));});

test('full archived gate reserializes a mutated object and restores fresh controls', () => {
    const m = archiveMatrix('win32');
    assert.equal(verifyArchive(m).complete, true);
    const value = m.files['http-codec-metadata-single-worker'].cases.find(value => value.id === 'codec-supp-raw-byob-65536');
    const document = JSON.parse(value.observed.text);
    document.state.reads.at(-1).backing[0] = false;
    value.observed.text = value.finalState.text = JSON.stringify(document);
    assert.throws(() => verifyArchive(m), 'the same report object must be serialized and rejected after mutation');
    assert.equal(verifyArchive(archiveMatrix('win32')).complete, true, 'the mutated input does not poison the immutable template');
});

for (const [name, mutate] of [
    ['missing MessagePort report', m => {delete m.files['message-port-order-managed'];}],
    ['MessagePort ordering mismatch', m => {m.files['message-port-order-single-worker'].roles.native.public.promiseOrder.reverse();}],
    ['stale MessagePort input', m => {m.files['message-port-order-static-pool'].inputs.fixture = '0'.repeat(64);}],
    ['forced MessagePort native shutdown', m => {m.files['message-port-order-single-worker'].roles.native.shutdown.forced = true;}],
]) test('full archived gate rejects ' + name, () => {
    const m = archiveMatrix(); mutate(m); assert.throws(() => verifyArchive(m));
});

assert.deepEqual(declaredControls,inventory.cases,'all original and supplemental controls remain declared exactly once');
assert.equal(registeredControls,controlShard==='all'?296:controlShard==='primary'?114:controlShard==='archives'?117:65,'all selected controls are registered, never skipped');

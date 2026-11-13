import assert from 'node:assert/strict';
import {isDeepStrictEqual} from 'node:util';
import {referenceBackendModes} from './workerd-platform.mjs';
import {assertFetchTransitionCertificates} from './workerd-fetch-transition-contract.mjs';
import {assertNativeFetchEnvelope,exactFields} from './workerd-native-fetch-contract.mjs';
export const nativeArgumentRoutes=Object.freeze(['getReader','direct','prototype','subclass','reflect']);
export const nativeArgumentWidths=Object.freeze({Int8Array:1,Uint8Array:1,Uint8ClampedArray:1,Int16Array:2,Uint16Array:2,
  Int32Array:4,Uint32Array:4,Float16Array:2,Float32Array:4,Float64Array:8,BigInt64Array:8,BigUint64Array:8,DataView:1});
export const nativeArgumentKinds=Object.freeze(nativeArgumentRoutes.flatMap(route=>Object.keys(nativeArgumentWidths)
  .flatMap(view=>['read','least'].map(method=>`native-arguments-${route}-${view}-${method}`))));
const scenarios=['capacity','capacity-first-count','zero','negative','signed-max','signed-min','range-high','range-low',
  'range-fraction','string-capacity','object-capacity','primitive-capacity','bigint','symbol','object-bigint','object-symbol',
  'zero-null','zero-nan','zero-infinity','zero-fraction','conversion-error','primitive-error','primitive-noncallable',
  'released-capacity','released-zero','released-object-capacity','released-conversion-error','release-during-conversion',
  'empty-capacity','empty-zero','detached-capacity','detach-during-conversion','invalid-view','invalid-view-conversion-error',
  'wrong-receiver','proxy-receiver','valid-default','valid-minimum','valid-fraction','valid-string'];
const invocation='Illegal invocation: function called with incorrect `this` reference. '
  +'See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.';
const payload=Array.from({length:16},(_,i)=>i+1);
const bytes=(offset,count,bufferBytes,content,backing,done=false)=>({done,type:'Uint8Array',byteOffset:offset,
  byteLength:count,bufferBytes,bytes:[...content],backing:[...backing]});
export function expectedNativeArgumentState(kind,challenge){
  assert.ok(nativeArgumentKinds.includes(kind));
  const [,,route,viewName,method]=kind.split('-'),width=nativeArgumentWidths[viewName];
  const rows=[...scenarios,...(method==='read'?['options-getter-error','options-null','options-primitive','getter-detaches-then-throws']:[])].map(scenario=>{
    const wrong=['wrong-receiver','proxy-receiver'].includes(scenario),invalid=scenario.startsWith('invalid-view');
    const getter=method==='read'&&!wrong&&!invalid&&!['options-null','options-primitive'].includes(scenario);
    const valueOf=['object-capacity','object-bigint','object-symbol','conversion-error','released-object-capacity','released-conversion-error',
      'release-during-conversion','detach-during-conversion'].includes(scenario)||scenario==='invalid-view-conversion-error'&&method==='least';
    const primitive=['primitive-capacity','primitive-error'].includes(scenario);
    const success=['valid-minimum','valid-fraction','valid-string','options-null'].includes(scenario)
      ||scenario==='valid-default'&&method==='read'
      ||['capacity','string-capacity','object-capacity','primitive-capacity'].includes(scenario)&&width<8;
    const detached=success||['detached-capacity','detach-during-conversion','getter-detaches-then-throws'].includes(scenario);
    const released=scenario.startsWith('released-')||scenario==='release-during-conversion';
    const row={scenario,events:[...(getter?['get-min']:[]),...(valueOf?['valueOf']:[]),...(primitive?['primitive:number']:[]),...(wrong?[]:['returned'])],
      response:{status:200,contentLength:'16',used:false,locked:false},
      construction:{instanceOfReader:true,constructorMatches:true,directPrototype:!['subclass','reflect'].includes(route),prototypeParent:['subclass','reflect'].includes(route)},
      afterCall:{bufferBytes:detached?0:32,viewBytes:invalid?null:detached&&viewName==='DataView'?{error:{name:'TypeError'}}:
        detached||scenario.startsWith('empty-')?0:16,used:success,locked:!released}};
    if(success)row.read={value:bytes(8,16,32,payload,[...Array(8).fill(165),...payload,...Array(8).fill(165)])};
    else{
      const callback=['conversion-error','released-conversion-error','options-getter-error','getter-detaches-then-throws'].includes(scenario)
        ||scenario==='invalid-view-conversion-error'&&method==='least';
      let message;
      if(callback)message='validation/'+challenge+'/'+scenario;
      else if(wrong)message=invocation;
      else if(invalid)message=`Failed to execute '${method==='read'?'read':'readAtLeast'}' on 'ReadableStreamBYOBReader': parameter ${method==='read'?1:2} is not of type 'ArrayBufferView'.`;
      else if(released)message='This ReadableStream reader has been released.';
      else if(scenario.startsWith('empty-')||['detached-capacity','detach-during-conversion'].includes(scenario))message='You must call read() on a "byob" reader with a positive-sized TypedArray object.';
      else if(scenario==='options-primitive')message="Failed to execute 'read' on 'ReadableStreamBYOBReader': parameter 2 is not of type 'ReadableStreamBYOBReaderReadOptions'.";
      else if(scenario.startsWith('range-'))message='Value out of range. Must be between -2147483648 and 2147483647 (inclusive).';
      else if(scenario==='bigint'||scenario==='object-bigint')message='Cannot convert a BigInt value to a number';
      else if(scenario==='symbol'||scenario==='object-symbol')message='Cannot convert a Symbol value to a number';
      else if(scenario==='primitive-error')message='Cannot convert object to primitive value';
      else if(scenario==='primitive-noncallable')message="'3' returned for property 'Symbol(Symbol.toPrimitive)' of object '#<Object>' is not a function";
      else if(scenario.startsWith('zero')||scenario==='valid-default')message='Requested invalid minimum number of bytes to read (0).';
      else message=`Minimum bytes to read (${({'capacity-first-count':'17',negative:'18446744073709551615','signed-max':'2147483647','signed-min':'18446744071562067968'})[scenario]??'24'}) exceeds size of buffer (16).`;
      const failure={type:'object',name:callback?'Error':'TypeError',message,primitive:null,isTypeError:!callback,isRangeError:false,sameSentinel:callback};
      if(wrong)row.synchronous=failure;else row.read={error:failure};
    }
    row.originalBacking=detached?null:Array(32).fill(165);
    if(!success)row.retry=bytes(0,16,16,payload,payload);
    row.remaining=[];row.eof=bytes(0,0,1,[],[0],true);row.cancelReturnedUndefined=true;row.lockedAfterRelease=false;row.bodyUsed=true;
    return row;
  });
  return {kind,challenge,rows,unexpected:null};
}
// Bound immutable input-model construction only. Every current report and
// state still receives a complete value comparison on every validation.
const stateTemplates=new Map();
const freezeModel=value=>{
  if(value&&typeof value==='object'&&!Object.isFrozen(value)){
    for(const child of Object.values(value))freezeModel(child);
    Object.freeze(value);
  }
  return value;
};
export function nativeArgumentStateTemplate(kind,challenge){
  assert.ok(nativeArgumentKinds.includes(kind));assert.equal(typeof challenge,'string');
  const key=JSON.stringify([kind,challenge]);
  if(!stateTemplates.has(key)){
    const value=freezeModel(expectedNativeArgumentState(kind,challenge));
    if(stateTemplates.size===32)stateTemplates.delete(stateTemplates.keys().next().value);
    stateTemplates.set(key,value);
  }
  return stateTemplates.get(key);
}
export function assertNativeArgumentState(state,kind,challenge){
  assert.ok(isDeepStrictEqual(state,nativeArgumentStateTemplate(kind,challenge)),'independent native argument, view, release, ownership and retry model');
  return state.rows.length;
}
export const nativeArgumentShardCases=Object.freeze(Object.fromEntries(nativeArgumentRoutes.map((route,index)=>
    [route,Object.freeze(nativeArgumentKinds.slice(index*26,(index+1)*26))])));
export const nativeArgumentCapacity=Object.freeze({channelsTotal:2,pendingDialsTotal:1});
export const nativeArgumentInputPaths=Object.freeze({runner:'tests/test_websocket_fetch_arguments.mjs',
    fixture:'tests/fixtures/workerd-fetch-arguments.mjs',upstream:'tests/helpers/ws_fetch_arguments_peer.mjs',
    upstreamBase:'tests/helpers/ws_fetch_transition_peer.mjs',controlPeer:'tests/helpers/ws_response_client.mjs',
    hostBackend:'tests/helpers/ws_host_backend.mjs',contract:'tools/workerd-fetch-arguments-contract.mjs',
    nativeContract:'tools/workerd-native-fetch-contract.mjs',certificateContract:'tools/workerd-fetch-transition-contract.mjs',
    listenerControl:'tests/helpers/workerd_listener_control.mjs',listenerContract:'tools/workerd-fetch-byob-contract.mjs',
});
export function assertNativeArgumentCase(value,mode,nonces=new Set()){
    assert.ok(nativeArgumentKinds.includes(value.kind));
    const state=JSON.parse(value.observed.text).state;
    const rows=assertNativeArgumentState(state,value.kind,value.challenge);
    const document=assertNativeFetchEnvelope(value,{mode,requests:rows,body:Buffer.from(payload),nonces});
    return {kind:value.kind,observations:rows,realm:document.realm};
}
export function validateNativeArgumentReport(report,expected){
    const shard=expected.shard??null,kinds=shard===null?nativeArgumentKinds:nativeArgumentShardCases[shard];assert.ok(kinds);
    exactFields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','certificateInputs','capacity','cases',...(shard===null?[]:['shard'])],'complete native argument report');
    assert.equal(report.schema,'capsid-fetch-arguments-v1');assert.equal(report.complete,true);assert.equal(report.runId,expected.runId);
    assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);if(shard!==null)assert.equal(report.shard,shard);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));assert.deepEqual(report.referenceLock,expected.lock);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.inputs,expected.inputs);assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(nativeArgumentInputPaths).sort());
    for(const hash of Object.values(report.inputs))assert.match(hash,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,
        worker:report.mode==='reference'?null:expected.worker});
    assert.deepEqual(report.capacity,report.mode==='reference'?null:nativeArgumentCapacity);
    assertFetchTransitionCertificates(report.certificateInputs);assert.deepEqual(report.cases.map(value=>value.kind),kinds);
    assert.equal(new Set(report.cases.map(value=>value.challenge)).size,kinds.length);
    const nonces=new Set(),checked=report.cases.map(value=>assertNativeArgumentCase(value,report.mode,nonces));
    assert.equal(new Set(checked.map(value=>value.realm)).size,kinds.length,'fresh native reader realms');
    return checked.map(({kind,observations})=>({kind,observations}));
}
export function compareNativeArgumentShardReports(reference,candidates,expected){
    function check(reports,mode){
        assert.deepEqual(reports.map(value=>value.shard),nativeArgumentRoutes,'every native argument route is required once');
        const checked=[];
        for(const report of reports){assert.equal(report.mode,mode);checked.push(...validateNativeArgumentReport(report,{...expected,shard:report.shard}));}
        assert.deepEqual(checked.map(value=>value.kind),nativeArgumentKinds);
        const cases=reports.flatMap(value=>value.cases);
        assert.equal(new Set(cases.map(value=>value.challenge)).size,130);
        assert.equal(new Set(cases.map(value=>JSON.parse(value.initial.text).realm)).size,130);
        assert.equal(checked.reduce((sum,value)=>sum+value.observations,0),5460);return checked;
    }
    const baseline=check(reference,'reference');
    assert.deepEqual(candidates.map(group=>group[0]?.mode).sort(),referenceBackendModes(expected.lock).sort());
    for(const group of candidates)assert.deepEqual(check(group,group[0].mode),baseline);
    return {complete:true,groupsPerBackend:130,observationsPerBackend:5460,shardsPerBackend:5,backends:candidates.length};
}

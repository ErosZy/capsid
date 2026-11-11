import assert from 'node:assert/strict';
import {test} from 'node:test';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {handlerErrorKinds, handlerErrorCaseIds, assertHandlerErrorCase,
    assertHandlerErrorObservations, assertHandlerErrorDiagnostics} from '../tools/workerd-handler-error-contract.mjs';

const kinds = ['Error','TypeError','RangeError','ReferenceError','SyntaxError','URIError','EvalError',
    'string','undefined','null','number','subclass','renamed-type','renamed-error','number-message',
    'empty-message','message-getter','message-getter-throws','custom-tostring','tostring-throws',
    'symbol','bigint','override-timer','override-hasown','async-error-listener',
    'microtask-message-mutation','microtask-name-mutation',
    'error-send','error-send-symbol','error-send-object','error-send-conversion-throws',
    'error-close','error-close-send','error-invalid-close','error-microtask-send',
    'getter-send','getter-close','getter-close-send',
    'object','object-primitive','object-tostring-throws','null-prototype','object-data',
    'object-cycle','object-getter','object-symbol-value','object-proxy','object-tostringtag','object-fake-error',
    'dom-exception','dom-custom-name','dom-number-message','dom-extra','dom-name-getter','dom-message-getter','dom-null-prototype','dom-fake','dom-combined-getters','aggregate-error','aggregate-data','aggregate-cycle','aggregate-errors-getter','error-cause','error-properties','error-cycle','error-fixed-stack','error-null-prototype','error-name-number','error-name-null','error-name-undefined','error-name-object','error-name-symbol','error-own-function','error-cycle-plain','error-ordered-getters','error-symbol-getter','override-string','name-getter-throws','array','function'];
const sample = (scenario = 'c17') => ({events: [
    {type:'message',readyState:1}, {type:'microtask',readyState:scenario === 'c17' ? 1 : 2},
    {type:'nested-microtask',readyState:scenario === 'c17' ? 1 : 2},
    scenario === 'c17' ? {type:'microtask-send',readyState:1,outcome:'accepted'} :
        {type:'microtask-send',readyState:2,outcome:'threw',name:'TypeError',message:"Can't call WebSocket send() after close()."},
    {type:'error',readyState:3,message:'Uncaught TypeError: handler-error/' + scenario,
        errorName:'TypeError',errorMessage:'handler-error/' + scenario,errorText:'TypeError: handler-error/' + scenario,
        errorType:'object',originalError:false,trusted:true,bubbles:false,cancelable:false},
],wire:{hex:scenario === 'c17' ? '810671756575656481046c617465' : '8108612d6265666f7265880603e8646f6e65',
    frames:scenario === 'c17' ? [{opcode:1,payload:'cXVldWVk'},{opcode:1,payload:'bGF0ZQ=='}] :
        [{opcode:1,payload:'YS1iZWZvcmU='},{opcode:8,payload:'A+hkb25l'}],peerTerminated:true,
    completion:'peer-ended',transportError:null,closeReplySent:false}});

test('literal independent80-kind and161-case inventory', () => {
    assert.deepEqual(handlerErrorKinds,kinds);
    assert.deepEqual(handlerErrorCaseIds,['c17','c18'].flatMap(scenario =>
        [...kinds,...(scenario==='c18'?['error-listener-throw']:[])].map(kind => scenario + '/' + kind)));
});
for (const scenario of ['c17','c18']) test(scenario + ' trusted error follows nested microtasks and retains exact wire', () => {
    const value = sample(scenario), before = structuredClone(value);
    assertHandlerErrorCase(scenario + '/TypeError',value);
    assert.deepEqual(value,before,'validation retains raw observations');
});
for (const [name, corrupt, diagnostic] of [
    ['missing microtask',value=>value.events.splice(1,1),/exact microtask order/],
    ['early CLOSED',value=>value.events[1].readyState=3,/exact microtask order/],
    ['missing nested microtask',value=>value.events.splice(2,1),/exact microtask order/],
    ['rejected accepted send',value=>value.events[3].outcome='threw',/exact microtask order/],
    ['missing error',value=>value.events.pop(),/exact microtask order/],
    ['duplicate error',value=>value.events.push({...value.events.at(-1)}),/exact microtask order/],
    ['error still CLOSING',value=>value.events.at(-1).readyState=2,/exact microtask order/],
    ['reused thrown object',value=>value.events.at(-1).originalError=true,/exact microtask order/],
    ['untrusted error',value=>value.events.at(-1).trusted=false,/exact microtask order/],
    ['wrong error type',value=>value.events.at(-1).errorName='Error',/exact microtask order/],
    ['wrong error message',value=>value.events.at(-1).message='bad',/exact microtask order/],
    ['missing late frame',value=>value.wire.hex='8106717565756564',/exact accepted wire bytes/],
    ['wrong complete frame',value=>value.wire.frames[1].payload='YmFk',/exact complete wire frames/],
    ['local disposal',value=>value.wire.completion='client-disposed',/no deadline or local disposal/],
    ['deadline',value=>value.wire.completion='deadline',/no deadline or local disposal/],
    ['fake peer terminal',value=>value.wire.peerTerminated=false,/actual peer terminal/],
    ['peer Close reply',value=>value.wire.closeReplySent=true,/no peer Close reply/],
]) test('rejects ' + name,()=>{
    const value=sample();corrupt(value);
    assert.throws(()=>assertHandlerErrorCase('c17/TypeError',value),diagnostic);
});
test('missing and incomplete case inventory cannot compare green',()=>{
    assert.throws(()=>assertHandlerErrorObservations(undefined),/observations required/);
    assert.throws(()=>assertHandlerErrorObservations({'c17/TypeError':sample()}),/complete161-case inventory/);
});
test('complete actual reference dataset validates all161 shared-process cases without rewriting raw data',()=>{
    const source=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-handler-error-report.json',import.meta.url),'utf8'));
    assert.equal(source.inputFixtureSHA256,'d60f9081508318ed30d0be2d332c912fbbb7422c8f8f5dec2145c1c94b76f1e0');
    assert.equal(createHash('sha256').update(fs.readFileSync(new URL('./fixtures/workerd-ws/api-compat.mjs',import.meta.url))).digest('hex'),
        source.inputFixtureSHA256,'actual fixture still matches the reviewed reference input');
    assert.deepEqual(source.referenceWitnesses.map(x=>[x.platform,x.binarySHA256,x.shutdown]),[
        ['win32','c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a',{code:null,signal:'SIGTERM',forced:false}],
        ['linux','50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd',{code:0,signal:null,forced:false}],
    ],'distinct actual platform witnesses keep their honest process terminals');
    const before=structuredClone(source.observations);
    assert.equal(Object.keys(assertHandlerErrorObservations(source.observations)).length,161);
    assert.deepEqual(source.observations,before);
});
for (const [kind, field, replacement] of [
    ['subclass','errorName','Error'], ['subclass','errorIsTypeError',true],
    ['renamed-type','errorName','TypeError'], ['renamed-error','errorIsTypeError',false],
    ['number-message','errorMessage','7'], ['empty-message','errorText','TypeError: '],
    ['message-getter','conversionCalls',[]], ['message-getter','conversionCalls',['message','message']],
    ['message-getter','conversionCalls',[{operation:'message',readyState:3,after:['message']}]],
    ['message-getter','conversionCalls',[{operation:'message',readyState:1,after:['message','microtask']}]],
    ['message-getter-throws','errorMessage','message-conversion-failed'],
    ['custom-tostring','conversionCalls',['toString']], ['tostring-throws','conversionCalls',['toString']],
    ['symbol','errorMessage','handler-symbol'], ['bigint','errorMessage','12345678901234567000'],
    ['override-timer','overrideCalls',['setTimeout']], ['override-hasown','overrideCalls',['hasOwn']],
    ['async-error-listener','errorIsTypeError',false],
    ['microtask-message-mutation','errorMessage','changed-after-throw'],
    ['microtask-name-mutation','errorName','ChangedAfterThrow'],
]) test(`rejects extended ${kind}/${field}=${JSON.stringify(replacement)}`,()=>{
    const actual=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-handler-error-report.json',import.meta.url),'utf8'));
    const value=structuredClone(actual.observations['c17/'+kind]);
    assertHandlerErrorCase('c17/'+kind,value);
    value.events.at(-1)[field]=replacement;
    assert.throws(()=>assertHandlerErrorCase('c17/'+kind,value),/exact microtask order/);
});
test('retained C17 transport is explicit local cleanup, not invented server EOF',()=>{
    const value=sample();
    Object.assign(value.wire,{peerTerminated:false,completion:'client-disposed',observedOpenMs:120});
    assertHandlerErrorCase('c17/error-listener-throw',value);
    const early=structuredClone(value);early.wire.observedOpenMs=0;
    assert.throws(()=>assertHandlerErrorCase('c17/error-listener-throw',early),/actual retained-transport observation/);
    const closed=structuredClone(value);closed.wire.peerTerminated=true;
    assert.throws(()=>assertHandlerErrorCase('c17/error-listener-throw',closed),/actual peer terminal/);
});

for (const [scenario, kind, description, corrupt, diagnostic] of [
    ['c17','error-send','rejecting a synchronous write',v=>v.events.at(-1).outcome='threw',/exact microtask order/],
    ['c17','error-send','dropping a synchronous wire write',v=>v.wire.hex='810671756575656481046c617465',/exact accepted wire bytes/],
    ['c18','error-send-symbol','masking conversion with closed state',v=>v.events.at(-1).message="Can't call WebSocket send() after close().",/exact microtask order/],
    ['c18','error-send-object','skipping conversion after close',v=>v.events.splice(-2,1),/exact microtask order/],
    ['c17','error-send-conversion-throws','swallowing conversion failure',v=>v.events.at(-1).outcome='accepted',/exact microtask order/],
    ['c17','error-close','swallowing the stored exception',v=>v.events.at(-1).outcome='accepted',/exact microtask order/],
    ['c17','error-close-send','setting outgoing closed after failed close',v=>v.events.at(-1).outcome='threw',/exact microtask order/],
    ['c17','error-invalid-close','skipping argument validation',v=>v.events.at(-1).name='TypeError',/exact microtask order/],
    ['c17','error-microtask-send','rejecting post-error send',v=>v.events.find(x=>x.type==='post-error-send').outcome='threw',/exact microtask order/],
    ['c17','error-microtask-send','writing post-error payload',v=>v.wire.hex+='8110706f73742d6572726f722d76616c7565',/exact accepted wire bytes/],
    ['c17','error-microtask-send','closing outgoing on ignored close',v=>v.events.at(-1).outcome='threw',/exact microtask order/],
    ['c17','getter-send','moving getter side effect past microtasks',v=>v.events.splice(1,1),/exact microtask order/],
    ['c17','getter-close','waiting for an absent Close reply',v=>v.wire.completion='deadline',/no deadline or local disposal/],
    ['c17','getter-close','adding a duplicate Close frame',v=>v.wire.frames.push(v.wire.frames.at(-1)),/exact complete wire frames/],
    ['c18','getter-close-send','skipping getter send conversion',v=>v.events.splice(3,1),/exact microtask order/],
]) test(`rejects ${scenario}/${kind}: ${description}`,()=>{
    const actual=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-handler-error-report.json',import.meta.url),'utf8'));
    const id=scenario+'/'+kind,value=structuredClone(actual.observations[id]);
    assertHandlerErrorCase(id,value);
    corrupt(value);
    assert.throws(()=>assertHandlerErrorCase(id,value),diagnostic);
});

const objectReport = () => JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-handler-error-report.json',import.meta.url),'utf8')).observations;
for (const [kind, description, corrupt] of [
    ['dom-exception','ordinary Error instead of native DOMException',e=>e.errorIsDOMException=false],
    ['dom-custom-name','public override replacing the private name',e=>e.errorName='VisibleDOM'],
    ['dom-number-message','public override replacing the private message',e=>e.errorMessage=7],
    ['dom-extra','retaining unsupported DOM payload',e=>e.errorGraph.properties.extra={value:{type:'number',value:7}}],
    ['dom-name-getter','missing metadata read',e=>e.conversionCalls.pop()],
    ['dom-message-getter','extra message read',e=>e.conversionCalls.push('dom-message')],
    ['dom-null-prototype','losing the native brand with a null prototype',e=>e.errorIsDOMException=false],
    ['dom-fake','granting a native brand by prototype',e=>e.errorIsDOMException=true],
    ['dom-combined-getters','wrong public getter order',e=>e.conversionCalls=['name','name','message']],
    ['aggregate-error','missing child native Error',e=>e.errorGraph.properties.errors.value.properties[0].value.error=false],
    ['aggregate-data','breaking errors/cause alias',e=>e.errorGraph.properties.errors.value.properties[0].value={ref:2}],
    ['aggregate-cycle','retaining a rejected native Error cycle',e=>e.errorGraph.properties.errors={value:{ref:0}}],
    ['aggregate-errors-getter','skipping the errors getter',e=>e.conversionCalls=[]],
    ['error-cause','non-enumerable reconstructed cause',e=>e.errorGraph.properties.cause.enumerable=false],
    ['error-properties','dropping a non-enumerable source property',e=>delete e.errorGraph.properties.hidden],
    ['error-cycle','preserving an Error self-reference',e=>e.errorGraph.properties.self={value:{ref:0}}],
    ['error-fixed-stack','preserving the source stack',e=>e.errorStack='fixed-stack'],
    ['error-name-number','stringifying the preserved raw name',e=>e.errorName='7'],
    ['error-name-null','discarding the preserved null name',e=>delete e.errorName],
    ['error-name-undefined','inventing an Error name',e=>e.errorName='Error'],
    ['error-name-object','using the converted name as a native tag',e=>e.errorIsTypeError=true],
    ['error-name-symbol','retaining the uncloneable name',e=>e.errorName='Symbol(name)'],
    ['error-own-function','retaining an uncloneable extra property',e=>e.errorGraph.properties.extra={accessor:true}],
    ['error-cycle-plain','retaining an indirect Error cycle',e=>e.errorGraph.properties.extra={value:{ref:0}}],
    ['error-ordered-getters','recursing before gathering sibling values',e=>e.conversionCalls=['first','nested','second']],
    ['error-symbol-getter','ignoring the symbol getter side effect',e=>e.conversionCalls=[]],
    ['override-string','calling the replaced String global',e=>e.overrideCalls=['String']],
    ['name-getter-throws','unlinked event message',e=>e.message='Uncaught Error: internal error; reference = 000000000000000000000000'],
    ['array','filling the source hole',e=>e.errorGraph.properties[1]={writable:true,enumerable:true,configurable:true,value:{type:'undefined'}}],
    ['function','changing the fallback function source',e=>e.errorMessage='function(){}'],
]) test(`enhanced error rejects ${kind}: ${description}`,()=>{
    const value=objectReport()['c17/'+kind];
    assertHandlerErrorCase('c17/'+kind,value);
    corrupt(value.events.at(-1));
    assert.throws(()=>assertHandlerErrorCase('c17/'+kind,value),/exact microtask order/);
});
test('diagnostic IDs are validated before normalization and raw reports stay intact',()=>{
    const raw=objectReport(),before=structuredClone(raw);
    const normalized=assertHandlerErrorObservations(raw);
    assert.equal(normalized['c17/name-getter-throws'].events.at(-1).errorMessage,'internal error; reference = <diagnostic-id>');
    assert.deepEqual(raw,before);
    const wrong=structuredClone(raw['c17/name-getter-throws']);wrong.events.at(-1).errorMessage='internal error; reference = z';
    assert.throws(()=>assertHandlerErrorCase('c17/name-getter-throws',wrong),/actual internal diagnostic reference/);
    const ids=['c17','c18'].map(s=>raw[s+'/name-getter-throws'].events.at(-1).errorMessage.slice(-24));
    assertHandlerErrorDiagnostics(raw,ids.join('\n'));
    assert.throws(()=>assertHandlerErrorDiagnostics(raw,''),/real log record/);
    const repeated=structuredClone(raw);
    repeated['c18/name-getter-throws'].events[4]=structuredClone(raw['c17/name-getter-throws'].events.at(-1));
    assert.throws(()=>assertHandlerErrorObservations(repeated),/fresh diagnostic references/);
});
for (const scenario of ['c17','c18']) {
    for (const kind of ['object-primitive','null-prototype','object-tostringtag'])
        test(`${scenario}/${kind} is a fresh ordinary object with ignored symbol keys`,()=>{
            const value=objectReport()[scenario+'/'+kind],event=value.events.at(-1);
            assertHandlerErrorCase(scenario+'/'+kind,value);
            assert.equal(event.message,'Uncaught #<Object>');
            assert.equal(event.errorIsError,false);
            assert.equal(Object.hasOwn(event,'errorName'),false);
            assert.equal(Object.hasOwn(event,'errorMessage'),false);
            assert.deepEqual(event.objectGraph,{id:0,array:false,prototype:'Object',error:false,symbols:0,properties:{}});
        });
    test(scenario+'/object-cycle retains aliases and self-reference as data properties',()=>{
        const value=objectReport()[scenario+'/object-cycle'],g=value.events.at(-1).objectGraph;
        assertHandlerErrorCase(scenario+'/object-cycle',value);
        assert.deepEqual(g.properties.right,{writable:true,enumerable:true,configurable:true,value:{ref:1}});
        assert.deepEqual(g.properties.self,{writable:true,enumerable:true,configurable:true,value:{ref:0}});
        assert.equal(g.properties.left.value.properties.x.value.value,7);
        const data=objectReport()[scenario+'/object-data'].events.at(-1).objectGraph;
        const array=data.properties.items.value;
        assert.equal(array.prototype,'Array');assert.equal(array.properties.length.value.value,5);
        assert.equal(Object.hasOwn(array.properties,1),false,'hole remains absent');
        assert.deepEqual(array.properties[2].value,{type:'undefined'},'explicit undefined remains present');
        assert.deepEqual(array.properties[3].value,{ref:1},'array item aliases the sibling object');
        assert.deepEqual(array.properties[4].value,{ref:2},'array self-reference');
        assert.equal(data.prototype,'Object');assert.equal(Object.hasOwn(data.properties,'__proto__'),true);
        assert.equal(Object.hasOwn(data.properties,'hidden'),false);
    });
    test(scenario+'/object-getter is read once before microtasks and becomes writable data',()=>{
        const value=objectReport()[scenario+'/object-getter'],event=value.events.at(-1);
        assertHandlerErrorCase(scenario+'/object-getter',value);
        assert.deepEqual(event.conversionCalls,[{operation:'detail',readyState:scenario==='c17'?1:2,after:['message']}]);
        assert.deepEqual(Object.keys(event.objectGraph.properties.detail).sort(),['configurable','enumerable','value','writable']);
        assert.equal(event.objectGraph.properties.detail.writable,true);
    });
    test(scenario+'/object-fake-error loses the supplied Error prototype',()=>{
        const value=objectReport()[scenario+'/object-fake-error'],event=value.events.at(-1);
        assertHandlerErrorCase(scenario+'/object-fake-error',value);
        assert.equal(event.errorIsError,false);assert.equal(event.errorIsTypeError,false);
        assert.equal(event.objectGraph.prototype,'Object');assert.equal(event.errorMessage,'fake-message');
    });
}
for (const [kind, description, corrupt] of [
    ['object','calling a custom conversion',e=>e.conversionCalls.push('toString')],
    ['object-primitive','calling Symbol.toPrimitive',e=>e.conversionCalls.push('primitive/string')],
    ['object-tostring-throws','calling a throwing conversion',e=>e.conversionCalls.push('toString')],
    ['null-prototype','rebuilding an Error',e=>e.errorIsError=true],
    ['object-tostringtag','retaining a symbol key',e=>e.objectGraph.symbols=1],
    ['object-data','dropping nested data',e=>delete e.objectGraph.properties.nested],
    ['object-data','filling an array hole',e=>e.objectGraph.properties.items.value.properties[1]={writable:true,enumerable:true,configurable:true,value:{type:'undefined'}}],
    ['object-data','dropping explicit undefined',e=>delete e.objectGraph.properties.items.value.properties[2]],
    ['object-data','treating __proto__ as a prototype setter',e=>e.objectGraph.prototype='other'],
    ['object-cycle','losing a shared reference',e=>e.objectGraph.properties.right.value={ref:7}],
    ['object-cycle','losing the self-reference',e=>e.objectGraph.properties.self.value={ref:1}],
    ['object-cycle','preserving the wrong descriptor',e=>e.objectGraph.properties.left.enumerable=false],
    ['object-cycle','reusing the original object',e=>e.originalError=true],
    ['object-getter','skipping the accessor',e=>e.conversionCalls=[]],
    ['object-getter','reading the accessor twice',e=>e.conversionCalls.push({...e.conversionCalls[0]})],
    ['object-getter','reading after error delivery',e=>e.conversionCalls[0].readyState=3],
    ['object-getter','retaining a readonly property',e=>e.objectGraph.properties.detail.writable=false],
    ['object-proxy','running an ownKeys trap',e=>e.conversionCalls.push('ownKeys')],
    ['object-fake-error','keeping TypeError brand',e=>e.errorIsTypeError=true],
    ['object-fake-error','keeping the source prototype',e=>e.objectGraph.prototype='other'],
]) test(`rejects ${kind}: ${description}`,()=>{
    const id='c17/'+kind,value=structuredClone(objectReport()[id]);
    assertHandlerErrorCase(id,value);corrupt(value.events.at(-1));
    assert.throws(()=>assertHandlerErrorCase(id,value),/exact microtask order/);
});

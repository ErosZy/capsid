import strictAssert from 'node:assert/strict';
import {createHash} from 'node:crypto';

const constructed = {status:101,statusText:'Switching Protocols',body:true,bodyUsed:false,contentType:null,endpoint:true};
const errors = {
    range:{error:'RangeError',message:'Responses with a WebSocket must have status code 101.'},
    body:{error:'TypeError',message:'Response with null body status (101, 204, 205, or 304) cannot have a body.'},
    socket:{error:'TypeError',message:"Incorrect type for the 'webSocket' field on 'InitializerDict': the provided value is not of type 'WebSocket'."},
};
const constructors = {
    missing:{error:'RangeError',message:'Responses may only be constructed with status codes in the range 200 to 599, inclusive.'},
    'wrong-object':errors.socket, 'wrong-status':errors.range, 'wrong-status-object':errors.socket,
    'wrong-status-100':errors.range, 'wrong-status-201':errors.range,
    'wrong-status-500':errors.range, 'default-status':errors.range,
    'body-empty':constructed, 'body-text':errors.body, 'body-zero':errors.body,
    'body-bytes-empty':constructed, 'body-buffer-empty':constructed,
    'body-blob-empty':constructed, 'body-blob-empty-type':constructed,
    'body-empty-explicit-type':{...constructed,contentType:'text/x-explicit'},
    'body-params-empty':constructed, 'body-boxed-empty':constructed,
    'body-stream-empty':errors.body, 'body-dataview-empty':constructed, 'body-view-empty-offset':constructed,
    'body-buffer-spoof-empty':errors.body, 'body-view-spoof-empty':errors.body,
    'body-blob-spoof-empty':errors.body, 'body-params-spoof-empty':errors.body,
    'body-params-spoof-nonempty':constructed, 'body-detached-buffer':constructed,
    'body-detached-view':constructed, 'body-detached-dataview':constructed,
    'body-undefined':constructed, unaccepted:constructed,
    clone:{error:'TypeError',message:'Cannot clone a response to a WebSocket handshake.'},
};
export function assertResponseConstructors(observed, assert = strictAssert) {
    assert.deepEqual(observed, constructors, 'all Response constructor, body and clone outcomes');
}

function keys(value, expected, assert) {
    assert.deepEqual(Object.keys(value).sort(), expected.toSorted());
}

function normalizeWire(wire, text, pending, assert, closeOnly = false) {
    keys(wire,['status','rawHeader','requestKey','rawTail','frames','peerTerminated','transportError','completion'],assert);
    assert.equal(wire.status,101,'Response commits 101 before coupling');
    assert.ok(typeof wire.rawHeader === 'string' && wire.rawHeader.length < 32768);
    const [status,...lines] = wire.rawHeader.split('\r\n');
    assert.equal(status,'HTTP/1.1 101 Switching Protocols');
    const headers = new Map();
    for (const line of lines) {
        const colon = line.indexOf(':');
        assert.ok(colon>0,'well-formed Response header');
        const name = line.slice(0,colon).toLowerCase(),value=line.slice(colon+1).trim();
        if (!headers.has(name)) headers.set(name,[]);
        headers.get(name).push(value);
    }
    assert.deepEqual(headers.get('upgrade'),['websocket']);
    assert.deepEqual(headers.get('connection')?.map(x=>x.toLowerCase()),['upgrade']);
    assert.match(wire.requestKey,/^[A-Za-z0-9+/]{22}==$/);
    assert.equal(Buffer.from(wire.requestKey,'base64').length,16);
    const accept = createHash('sha1').update(wire.requestKey+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    assert.deepEqual(headers.get('sec-websocket-accept'),[accept],'actual response matches the independent client nonce');
    assert.deepEqual(headers.get('sec-websocket-protocol'),['chat']);
    assert.deepEqual(headers.get('x-response-fixture'),['yes']);
    for (const name of ['content-length','transfer-encoding','sec-websocket-extensions'])
        assert.equal(headers.has(name),false,`unexpected ${name} on fixture Upgrade`);
    assert.equal(wire.peerTerminated,!pending,'peer termination is observed before client disposal');
    if (pending) {
        assert.equal(wire.completion,null);
        assert.equal(wire.transportError,null);
    } else {
        assert.ok(wire.completion === 'peer-ended' && wire.transportError === null ||
            wire.completion === 'peer-reset' && wire.transportError === 'ECONNRESET',
            'only observed peer EOF/reset is a transport terminal');
    }
    const close = {opcode:8,payload:Buffer.from([3,232,...Buffer.from('done')]).toString('base64')};
    const frames = text === null ? (closeOnly ? [close] : []) : [
        {opcode:1,payload:Buffer.from('reply:'+text).toString('base64')},
        close,
    ];
    assert.deepEqual(wire.frames,frames,'complete Response message/Close frames');
    const bytes = Buffer.concat(frames.map(({opcode,payload})=>{
        const data=Buffer.from(payload,'base64');
        return Buffer.concat([Buffer.from([0x80|opcode,data.length]),data]);
    }));
    assert.equal(wire.rawTail,bytes.toString('base64'),'no unparsed or appended bytes after 101');
    // Raw headers, nonce, and EOF-versus-reset remain archived. Validate them
    // first, then compare their semantic result across independent processes.
    return {status:wire.status,frames:structuredClone(wire.frames),peerTerminated:wire.peerTerminated};
}

export function assertResponseObservation(observation, assert = strictAssert) {
    keys(observation,['constructors','controls','aborted','delayed','delayedClose','crossContext','outbound','healthy'],assert);
    assertResponseConstructors(observation.constructors,assert);
    const normalized = structuredClone(observation);
    assert.deepEqual(observation.controls.map(x=>x.phase),['before','after']);
    normalized.controls=observation.controls.map(control=>{
        keys(control,['phase','simultaneous','connections'],assert);
        assert.equal(control.simultaneous,true,'both service slots are live together');
        assert.equal(control.connections.length,2);
        return {...control,connections:control.connections.map((wire,index)=>
            normalizeWire(wire,control.phase+':'+index,false,assert))};
    });
    const required=[];
    for(let iteration=0;iteration<6;++iteration)
        for(const kind of ['accepted','both','closed'])required.push({kind,iteration});
    assert.deepEqual(observation.aborted.map(({kind,iteration})=>({kind,iteration})),required,
        'all three coupling failures and six quota-reuse rounds');
    normalized.aborted=observation.aborted.map(record=>{
        keys(record,['kind','iteration','wire'],assert);
        return {...record,wire:normalizeWire(record.wire,null,false,assert)};
    });
    keys(observation.delayed,['before','beforeWire','release','wire','after'],assert);
    assert.deepEqual(observation.delayed.before,{accepted:false,events:[]},'no event before peer accept');
    assert.deepEqual(observation.delayed.release,{status:200,text:'released'});
    assert.deepEqual(observation.delayed.after,{accepted:true,events:[
        {type:'message',data:'early'},{type:'close',code:1000,clean:true},
    ]});
    normalized.delayed.beforeWire=normalizeWire(observation.delayed.beforeWire,null,true,assert);
    normalized.delayed.wire=normalizeWire(observation.delayed.wire,'early',false,assert);
    keys(observation.delayedClose,['before','beforeWire','release','wire','after'],assert);
    assert.deepEqual(observation.delayedClose.before,{accepted:false,events:[]},'DATA and Close wait for peer accept');
    assert.deepEqual(observation.delayedClose.release,{status:200,text:'released'});
    assert.deepEqual(observation.delayedClose.after,{accepted:true,events:[
        {type:'message',data:'early'},{type:'close',code:1000,clean:true},
    ]},'clean Close does not discard preceding DATA');
    normalized.delayedClose.beforeWire=normalizeWire(observation.delayedClose.beforeWire,null,true,assert);
    normalized.delayedClose.wire=normalizeWire(observation.delayedClose.wire,null,false,assert,true);
    assert.deepEqual(observation.crossContext,{status:500,bodyComplete:true,bodyNonempty:true});
    assert.deepEqual(observation.outbound.map(x=>x.kind),['outbound-open','outbound-connecting']);
    normalized.outbound=observation.outbound.map(record=>{
        keys(record,['kind','wire','upstreamStopped'],assert);
        assert.equal(record.upstreamStopped,true);
        return {...record,wire:normalizeWire(record.wire,null,false,assert)};
    });
    assert.deepEqual(observation.healthy,{status:200,text:'healthy'});
    return normalized;
}

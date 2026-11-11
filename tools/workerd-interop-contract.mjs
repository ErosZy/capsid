import assert from 'node:assert/strict';
import {referenceBackendModes} from './workerd-platform.mjs';
import {createHash} from 'node:crypto';

export const interopCases=Object.freeze([
    ...['node','python'].flatMap(client=>['ws','wss'].flatMap(transport=>['exchange','server-close'].map(operation=>
        Object.freeze({name:`${client}/${transport}/${operation}`,direction:'inbound',client,transport,operation})))),
    ...['ws','wss'].map(transport=>Object.freeze({name:`python/${transport}/fragment-ping`,direction:'inbound',client:'python',transport,operation:'fragment-ping'})),
    ...['node','python'].flatMap(client=>['untrusted','wrongHost'].map(transport=>
        Object.freeze({name:`${client}/tls/${transport}`,direction:'inbound',client,transport,operation:'tls-reject'}))),
    ...['node','python'].flatMap(peer=>['ws','wss','untrusted','wrongHost'].map(transport=>Object.freeze({name:`outbound/${peer}/${transport}`,direction:'outbound',peer,transport}))),
]);
export const interopInputPaths=Object.freeze({runner:'tests/test_websocket_interop.mjs',pythonProgram:'tests/helpers/ws_interop_python.py',
    nodeProgram:'tests/helpers/ws_interop_node.mjs',fixture:'tests/fixtures/websocket-interop.mjs',
    clientLock:'tests/fixtures/websocket-interop-lock.json',contract:'tools/workerd-interop-contract.mjs',hostBackend:'tests/helpers/ws_host_backend.mjs',
    nodeSDK:'tests/helpers/ws_interop_node_sdk.mjs',nodePeer:'tests/helpers/ws_interop_node_peer.mjs'});
function keys(value,names) {assert.ok(value&&typeof value==='object');assert.deepEqual(Object.keys(value).sort(),names.toSorted());}
const digest=text=>createHash('sha256').update(text).digest('hex');
function payloads(challenge,fragmented) {
    const result=[{type:'text',length:Buffer.byteLength('challenge:'+challenge),sha256:digest('challenge:'+challenge)},
        {type:'text',length:13,sha256:'98beac0e4d0c2f00e2be1393181c39004ca9dc1123772d87f402d6876a66594b'},
        {type:'binary',length:0,sha256:'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'},
        {type:'binary',length:180000,sha256:'f746a05272efcf03fdeb5d9dd3456ba1d03789820437246c0c76116efdd535f3'}];
    if(fragmented)result.push({type:'text',length:11,sha256:'6a4fc89ec949d739959c84176fea12a1af9a6ca3df2274a37e7546c758ff9e34'},
        {type:'binary',length:60240,sha256:'ab1e4a9756ed274cd56a8ce7f139b2f279648b3fb627b62807265f6a8e523e26'});
    return result;
}
function workerMessages(challenge) {
    return [{type:'text',text:'challenge:'+challenge},{type:'text',text:'hello\0界😀'},
        {type:'binary',length:0,first:[],last:[]},
        {type:'binary',length:180000,first:[29,46,63,80,97,114,131,148],last:[203,220,237,3,20,37,54,71]},
        {type:'text',text:'分片\0😀'},{type:'binary',length:60240,first:[0,1,2,3,4,5,6,7],last:[243,244,245,246,247,248,249,250]}];
}
function pythonIdentity(lock) {return {implementation:lock.python.implementation,python:lock.python.version,
    websockets:lock.websockets.version,sourceFiles:lock.websockets.sourceFiles,sourceSHA256:lock.websockets.sourceSHA256};}
function endpointSet(endpoints) {
    keys(endpoints,['inbound','outbound']);
    keys(endpoints.outbound,['node','python']);
    for(const set of [endpoints.inbound,...Object.values(endpoints.outbound)]) {
        keys(set,['ws','wss','untrusted','wrongHost']);
        for(const [kind,value] of Object.entries(set)) {
            const url=new URL(value);assert.equal(url.protocol,kind==='ws'?'ws:':'wss:');
            assert.equal(url.hostname,kind==='ws'||kind==='wrongHost'?'127.0.0.1':'localhost');
            assert.ok(Number(url.port)>0&&Number(url.port)<=65535);assert.equal(url.pathname,'/');
            assert.equal(value,`${url.protocol}//${url.host}`);
        }
        assert.equal(new URL(set.wss).port,new URL(set.wrongHost).port);
        assert.notEqual(new URL(set.wss).port,new URL(set.untrusted).port);
    }
}

export function validateInteropReport(report,expected) {
    keys(report,['schema','complete','runId','mode','referenceLock','clientLock','inputs','binaries','clients','endpoints','cases','peers','backendStderr','healthy','shutdown']);
    assert.equal(typeof report.backendStderr,'string');
    assert.equal(report.schema,'capsid-websocket-interop-v1');assert.equal(report.complete,true);
    assert.equal(typeof expected.runId,'string');assert.ok(expected.runId.length>0);assert.equal(report.runId,expected.runId,'interop report belongs to this run');
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode),'supported reference platform mode');assert.deepEqual(report.referenceLock,expected.lock);assert.deepEqual(report.clientLock,expected.clientLock);
    keys(report.inputs,Object.keys(interopInputPaths));
    for(const name of Object.keys(interopInputPaths)) {
        assert.match(expected.inputs[name],/^[0-9a-f]{64}$/);assert.equal(report.inputs[name],expected.inputs[name],`interop ${name} source identity`);
    }
    keys(report.binaries,['host','worker']);assert.equal(report.binaries.host,report.mode==='reference'?expected.lock.binary_sha256:expected.host);
    assert.equal(report.binaries.worker,report.mode==='reference'?null:expected.worker);
    const identities={node:{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version,
        ws:expected.clientLock.ws.version,sourceFiles:expected.clientLock.ws.sourceFiles,sourceSHA256:expected.clientLock.ws.sourceSHA256},python:pythonIdentity(expected.clientLock)};
    assert.deepEqual(report.clients,identities,'independent client versions and Python implementation digest');
    endpointSet(report.endpoints);
    assert.deepEqual(report.cases.map(x=>x.name),interopCases.map(x=>x.name),'complete ordered independent-client scenario inventory');
    const normalized=[];
    for(let index=0;index<interopCases.length;index++) {
        const spec=interopCases[index],entry=report.cases[index],value=entry.observed,challenge=report.runId+':'+spec.name;
        keys(entry,['name','observed','tcpAccepted']);
        if(spec.direction==='inbound') {
            const isFailure=spec.operation==='tls-reject',fragmented=spec.operation==='fragment-ping';
            keys(value,['identity','uri','case','messages','pingPong',...(isFailure?['failure']:['protocol','close'])]);
            assert.deepEqual(value.identity,identities[spec.client]);assert.equal(value.case,spec.name);
            assert.equal(value.uri,report.endpoints.inbound[spec.transport]+'/@capsid/orders/inbound?'+new URLSearchParams({case:spec.name}));
            assert.equal(entry.tcpAccepted,spec.transport==='ws'?null:1,'actual client TCP attempts reach the intended fixture');
            if(isFailure) {
                assert.deepEqual(value.messages,[]);assert.equal(value.pingPong,0);
                if(spec.client==='python') {
                    keys(value.failure,['opened','certificateRejected','errorName','verifyCode','verifyMessage']);
                    assert.equal(value.failure.opened,false);assert.equal(value.failure.certificateRejected,true);
                    assert.equal(value.failure.errorName,'SSLCertVerificationError');
                    assert.equal(value.failure.verifyCode,spec.transport==='untrusted'?18:64);
                    assert.equal(value.failure.verifyMessage,spec.transport==='untrusted'?'self-signed certificate':"IP address mismatch, certificate is not valid for '127.0.0.1'.");
                } else {
                    keys(value.failure,['opened','errors','close']);assert.equal(value.failure.opened,false);
                    assert.equal(value.failure.errors.length,1);assert.equal(value.failure.errors[0].name,'Error');
                    assert.equal(value.failure.errors[0].message,spec.transport==='untrusted'
                        ?'self-signed certificate; if the root CA is installed locally, try running Node.js with --use-system-ca'
                        :"Hostname/IP does not match certificate's altnames: IP: 127.0.0.1 is not in the cert's list: ");
                    assert.deepEqual(value.failure.close,{code:1006,reason:'',clean:false});
                    assert.equal(value.failure.errors[0].code,spec.transport==='untrusted'?'DEPTH_ZERO_SELF_SIGNED_CERT':'ERR_TLS_CERT_ALTNAME_INVALID');
                }
                normalized.push({name:spec.name,certificateRejected:true});
            } else {
                assert.equal(value.protocol,'chat');assert.equal(value.pingPong,fragmented?2:0);
                assert.deepEqual(value.messages,spec.operation==='server-close'?[]:payloads(challenge,fragmented),'actual independent client payload types and SHA256');
                assert.deepEqual(value.close,{code:spec.operation==='server-close'?3001:1000,reason:spec.operation==='server-close'?'server\0终':'client\0终',clean:true});
                normalized.push({name:spec.name,messages:value.messages,pingPong:value.pingPong,close:value.close});
            }
        } else {
            keys(value,['target','events']);
            assert.equal(value.target,report.endpoints.outbound[spec.peer][spec.transport]+'/echo?'+new URLSearchParams({case:spec.name,challenge}));
            assert.equal(entry.tcpAccepted,spec.transport==='ws'?null:1,'native outbound transport reaches the independent peer');
            const failure=['untrusted','wrongHost'].includes(spec.transport);
            const events=[{type:'construct',state:0}];
            if(failure)events.push({type:'error',state:3,errorName:'Error',isError:true},{type:'close',state:3,code:1006,reason:'Failed to establish websocket connection',clean:false});
            else events.push({type:'open',state:1,protocol:'chat'},...workerMessages(challenge).map(data=>({type:'message',data})),{type:'close',state:3,code:1000,reason:'remote\0终',clean:true});
            assert.deepEqual(value.events,events,'worker events for the independent protocol peer');normalized.push({name:spec.name,events:value.events});
        }
    }
    keys(report.peers,['node','python']);
    const normalizedPeers={};
    for(const [name,peer] of Object.entries(report.peers)) {
      if(name==='python') {
        keys(peer,['type','active','records','exit','signal','stderr']);assert.equal(typeof peer.stderr,'string');assert.equal(peer.type,'stopped');
        assert.equal(peer.exit,0);assert.equal(peer.signal,null);
      }else {keys(peer,['identity','active','records']);assert.deepEqual(peer.identity,identities.node);}
      assert.equal(peer.active,0);
      assert.deepEqual(peer.records.map(x=>x.case),[`outbound/${name}/ws`,`outbound/${name}/wss`],'only verified TLS connections reach the independent WebSocket handler');
      for(const record of peer.records) {
        keys(record,['case','protocol','extensions','messages','pingPong','sni','close']);assert.equal(record.protocol,'chat');
        assert.equal(record.extensions,name==='python'?0:'');
        assert.equal(record.sni,record.case.endsWith('/wss')?'localhost':null);assert.equal(record.pingPong,2);
        assert.deepEqual(record.messages,payloads(report.runId+':'+record.case,true),'Python peer independently verifies complete echoed bytes');
        assert.deepEqual(record.close,{code:1000,reason:'remote\0终',clean:true});
      }
      normalizedPeers[name]=peer.records.map(record=>({...structuredClone(record),extensions:0}));
    }
    assert.deepEqual(report.healthy,{status:200,text:'healthy'});
    keys(report.shutdown,['code','signal','forced']);assert.equal(report.shutdown.forced,false,'no forced backend shutdown');
    if(report.mode==='reference')assert.ok(report.shutdown.code===0&&report.shutdown.signal===null||report.shutdown.code===null&&report.shutdown.signal==='SIGTERM');
    else assert.deepEqual(report.shutdown,{code:0,signal:null,forced:false});
    return {cases:structuredClone(normalized),peers:normalizedPeers,healthy:structuredClone(report.healthy)};
}

export function compareInteropReports(reference,candidates,expected) {
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(x=>x.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateInteropReport(reference,expected);
    for(const report of candidates)assert.deepEqual(validateInteropReport(report,expected),baseline,`${report.mode}: independent-client observations`);
    return {complete:true,casesPerBackend:interopCases.length,backends:1+candidates.length,clients:['node-ws','python-websockets'],peerSessionsPerBackend:{node:2,python:2}};
}

import assert from 'node:assert/strict';
import {referenceBackendModes} from './workerd-platform.mjs';

export const uuidPattern=/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
// Observed before the Capsid correction, on the locked reference for all
// send/readyState/close probes. Do not derive this value from candidate reports.
export const crossRequestMessage='Cannot perform I/O on behalf of a different request. '
    +'I/O objects (such as streams, request/response bodies, and others) created in the context '
    +"of one request handler cannot be accessed from a different request's handler. "
    +'This is a limitation of Cloudflare Workers which allows us to improve overall performance. '
    +'(I/O type: Native)';
function keys(value,names) {
    assert.ok(value&&typeof value==='object');
    assert.deepEqual(Object.keys(value).sort(),names.toSorted());
}
export function assertFanoutFixtureObservation(value) {
    keys(value,['identities','sender','peerMessages','survivor','closes']);
    assert.equal(value.identities.length,2,'fanout records both actual Worker identities');
    for(const identity of value.identities) {
        keys(identity,['instance','connection']);
        assert.match(identity.instance,uuidPattern);assert.match(identity.connection,uuidPattern);
    }
    assert.equal(value.identities[0].instance,value.identities[1].instance,'fixture peers share a Worker');
    assert.notEqual(value.identities[0].connection,value.identities[1].connection,'distinct actual connections');
    assert.equal(value.sender,'fanout:blocked');assert.deepEqual(value.peerMessages,[]);
    assert.equal(value.survivor,'fanout:blocked');
    assert.deepEqual(value.closes,[0,1].map(()=>({code:1000,reason:'',wasClean:true})));
    return {identities:[{instance:0,connection:0},{instance:0,connection:1}],
        sender:value.sender,peerMessages:[],survivor:value.survivor,closes:structuredClone(value.closes)};
}

export const fanoutInputPaths=Object.freeze({runner:'tests/test_workerd_fanout.mjs',
    fixture:'tests/fixtures/workerd-ws/fanout.mjs',contract:'tools/workerd-fanout-contract.mjs',
    hostBackend:'tests/helpers/ws_host_backend.mjs',nodeSDK:'tests/helpers/ws_interop_node_sdk.mjs'});
export const fanoutCases=Object.freeze(['same','cross'].flatMap(kind=>['send','readyState','close'].flatMap(operation=>
    [0,1].flatMap(group=>[0,1].map(within=>`${kind}/${operation}/${group}/${within}`)))));
export const fanoutOverlapCases=Object.freeze(['send','readyState','close'].flatMap(operation=>
    [0,1].map(group=>`overlap/${operation}/${group}`)));

export function validateFanoutReport(report,expected) {
    keys(report,['schema','complete','runId','mode','referenceLock','clientLock','inputs','binaries','client',
        'topology','endpoints','discovery','selected','extraCloses','baseline','probes','overlap','echoes',
        'memberCloses','afterClose','finalCloses','unexpectedMessages','healthy','shutdowns','backendStderr']);
    assert.equal(report.schema,'capsid-workerd-fanout-v1');assert.equal(report.complete,true);
    assert.equal(report.runId,expected.runId);assert.ok(report.runId);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode),'supported reference platform mode');
    assert.deepEqual(report.referenceLock,expected.lock);assert.deepEqual(report.clientLock,expected.clientLock);
    assert.deepEqual(report.inputs,expected.inputs);keys(report.inputs,Object.keys(fanoutInputPaths));
    for(const digest of Object.values(report.inputs))assert.match(digest,/^[0-9a-f]{64}$/);
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,
        worker:report.mode==='reference'?null:expected.worker});
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version,
        ws:expected.clientLock.ws.version,sourceFiles:expected.clientLock.ws.sourceFiles,sourceSHA256:expected.clientLock.ws.sourceSHA256});
    const hostCount=report.mode==='single-worker'?2:1;
    assert.deepEqual(report.topology,{workers:2,hostProcesses:hostCount,referenceServices:report.mode==='reference'?2:0});
    assert.equal(report.endpoints.length,['reference','single-worker'].includes(report.mode)?2:1);
    assert.equal(new Set(report.endpoints).size,report.endpoints.length);
    for(const endpoint of report.endpoints) {
        const url=new URL(endpoint);assert.equal(url.protocol,'ws:');assert.equal(url.hostname,'127.0.0.1');
        assert.ok(Number(url.port)>0&&Number(url.port)<=65535);assert.equal(endpoint,`ws://${url.host}/@capsid/orders/fanout`);
    }
    const discovered=new Map(),groups=new Map();
    assert.ok(report.discovery.length>=4&&report.discovery.length<=64,'bounded actual connection discovery');
    for(const [index,entry] of report.discovery.entries()) {
        keys(entry,['endpoint','identity']);assert.ok(report.endpoints.includes(entry.endpoint));
        const value=entry.identity;keys(value,['op','challenge','instance','connection','members']);
        assert.equal(value.op,'identity');assert.equal(value.challenge,report.runId+':identity:'+index);
        assert.match(value.instance,uuidPattern);assert.match(value.connection,uuidPattern);
        assert.ok(!discovered.has(value.connection),'connection identities never repeat');
        discovered.set(value.connection,value.instance);
        if(!groups.has(value.instance))groups.set(value.instance,[]);
        groups.get(value.instance).push(value.connection);
        assert.deepEqual(value.members.toSorted(),groups.get(value.instance).toSorted(),'room membership follows actual instance identity');
    }
    assert.equal(groups.size,2,'two actual Worker instance groups');
    assert.ok([...groups.values()].every(ids=>ids.length>=2),'two real connections per Worker');
    const selected=[...groups.values()].flatMap(ids=>ids.slice(0,2));
    assert.deepEqual(report.selected,selected,'selected same/different Worker groups');
    const extras=[...discovered.keys()].filter(id=>!selected.includes(id));
    function closes(records,ids,reason) {
        assert.deepEqual(records,ids.map(connection=>({connection,code:1000,reason,clean:true})),'actual reciprocal Close frames');
    }
    closes(report.extraCloses,extras,'fanout-extra');
    function stats(entries,ids,stage,membership) {
        assert.equal(entries.length,ids.length);
        for(const [index,observations] of entries.entries()) {
            const connection=ids[index],instance=discovered.get(connection);
            assert.ok(observations.length>0&&observations.length<=128,'bounded room cleanup observations');
            for(const [attempt,value] of observations.entries()) {
                keys(value,['op','challenge','instance','connection','members']);
                assert.equal(value.op,'stats');assert.equal(value.challenge,`${report.runId}:${stage}:${index}:${attempt}`);
                assert.equal(value.instance,instance);assert.equal(value.connection,connection);
                assert.equal(new Set(value.members).size,value.members.length);
                assert.ok(value.members.every(id=>discovered.get(id)===instance),'no foreign Worker room membership');
                assert.ok(value.members.includes(connection));
            }
            assert.deepEqual(observations.at(-1).members.toSorted(),membership(instance).toSorted(),'closed connections leave the application room');
        }
    }
    stats(report.baseline,selected,'baseline',instance=>selected.filter(id=>discovered.get(id)===instance));
    assert.deepEqual(report.probes.map(p=>p.name),fanoutCases,'complete same/different Worker probe inventory');
    const normalized=[];
    for(const entry of report.probes) {
        keys(entry,['name','sender','target','response']);
        const [kind,operation,groupText,withinText]=entry.name.split('/');
        const same=kind==='same',group=Number(groupText),within=Number(withinText),slot=group*2+within;
        const sender=selected[slot],target=selected[same?group*2+1-within:(1-group)*2+within];
        assert.equal(entry.sender,sender);assert.equal(entry.target,target);
        const value=entry.response;keys(value,['op','operation','challenge','instance','connection','attempts']);
        assert.equal(value.operation,operation);
        assert.equal(value.op,'probe');assert.equal(value.challenge,`${report.runId}:${entry.name}:\0界😀`);
        assert.equal(value.instance,discovered.get(sender));assert.equal(value.connection,sender);
        if(same) {
            assert.equal(value.attempts.length,1,'same Worker reaches the other request socket');
            const attempt=value.attempts[0];keys(attempt,['connection','outcome','errorName','message']);
            assert.equal(attempt.connection,target);assert.equal(attempt.outcome,'blocked');
            assert.equal(attempt.errorName,'Error');
            assert.equal(attempt.message,crossRequestMessage,'pinned Native I/O ownership diagnostic');
        } else assert.deepEqual(value.attempts,[],'different Worker does not share the application room');
        normalized.push({name:entry.name,attempts:same?'different-request-block':'separate-room'});
    }
    assert.deepEqual(report.overlap.map(p=>p.name),fanoutOverlapCases,'all overlapping-handler ownership probes');
    for(const entry of report.overlap) {
        keys(entry,['name','sender','target','heldBefore','probe','release','holdReply']);
        const [,operation,groupText]=entry.name.split('/'),group=Number(groupText);
        const sender=selected[group*2],target=selected[group*2+1];
        assert.equal(entry.sender,sender);assert.equal(entry.target,target);
        assert.ok(entry.heldBefore.length>0&&entry.heldBefore.length<=128);
        for(const [index,value] of entry.heldBefore.entries()) {
            assert.deepEqual(value,{op:'held',challenge:`${report.runId}:${entry.name}:held:${index}`,
                instance:discovered.get(sender),connection:sender,held:value.held});
            assert.ok(value.held.length<=1&&value.held.every(id=>id===target));
        }
        assert.deepEqual(entry.heldBefore.at(-1).held,[target],'target handler is actually pending during the foreign operation');
        assert.deepEqual(entry.probe,{op:'probe',operation,challenge:`${report.runId}:${entry.name}:\0界😀`,
            instance:discovered.get(sender),connection:sender,attempts:[{connection:target,outcome:'blocked',errorName:'Error',message:crossRequestMessage}]});
        assert.deepEqual(entry.release,{op:'release',challenge:`${report.runId}:${entry.name}:release`,
            instance:discovered.get(sender),connection:sender,released:true});
        assert.deepEqual(entry.holdReply,{op:'hold',challenge:`${report.runId}:${entry.name}:hold`,
            instance:discovered.get(target),connection:target});
    }
    assert.equal(report.echoes.length,4);
    for(const [index,value] of report.echoes.entries()) {
        keys(value,['op','challenge','instance','connection']);assert.equal(value.op,'echo');
        assert.equal(value.challenge,`${report.runId}:echo:${index}`);assert.equal(value.connection,selected[index]);
        assert.equal(value.instance,discovered.get(selected[index]));
    }
    closes(report.memberCloses,[selected[0],selected[2]],'fanout-member');
    stats(report.afterClose,[selected[1],selected[3]],'after-close',instance=>selected.filter((id,i)=>i%2===1&&discovered.get(id)===instance));
    closes(report.finalCloses,[selected[1],selected[3]],'fanout-done');
    assert.deepEqual(report.unexpectedMessages,Array.from(discovered.keys(),connection=>({connection,messages:[]})));
    assert.deepEqual(report.healthy,report.endpoints.map(()=>({status:200,text:'healthy'})));
    assert.equal(report.shutdowns.length,hostCount);assert.equal(report.backendStderr.length,hostCount);
    assert.ok(report.backendStderr.every(v=>typeof v==='string'));
    for(const shutdown of report.shutdowns) {
        keys(shutdown,['code','signal','forced']);assert.equal(shutdown.forced,false);
        if(report.mode==='reference')assert.ok(shutdown.code===0&&shutdown.signal===null||shutdown.code===null&&shutdown.signal==='SIGTERM');
        else assert.deepEqual(shutdown,{code:0,signal:null,forced:false});
    }
    return {workers:2,connectionsPerWorker:2,probes:normalized,overlap:fanoutOverlapCases,echoes:4,closed:true};
}

export function compareFanoutReports(reference,candidates,expected) {
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(r=>r.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateFanoutReport(reference,expected);
    for(const report of candidates)assert.deepEqual(validateFanoutReport(report,expected),baseline);
    return {complete:true,backends:1+candidates.length,workersPerBackend:2,probeCasesPerBackend:fanoutCases.length,
        overlappingHandlerCasesPerBackend:fanoutOverlapCases.length};
}

import assert from 'node:assert/strict';
import {isDeepStrictEqual} from 'node:util';
import {cancelBindingKinds,cancelBindingRoutes,cancelBindingViews,cancelBindingMethods,cancelBindingPacket,cancelBindingStateTemplate} from './binding-fetch-cancel-input.mjs';
// Actual fixed workerd finite-body User I/O captures are revalidated. The
// BindingID-owned pool and RPC cancellation policy are modeled separately.
import './binding-fetch-body-contract.mjs';
const equal=(actual,expected,message)=>assert.ok(isDeepStrictEqual(actual,expected),message??'complete current typed value');
const fields=(value,names)=>{assert.ok(value&&typeof value==='object'&&!Array.isArray(value));equal(Object.keys(value).sort(),names.slice().sort());};
export function assertCancelBindingCase(item,kind,route,view,method){
    fields(item,['kind','route','view','method','events','port','request1','afterRequest1','request1SampleSequence','beforeRequest2','cancelSampleSequence','request2','beforeStop','finalSampleSequence','child','workerExit','stdout','stderr','finalSources']);
    equal([item.kind,item.route,item.view,item.method],[kind,route,view,method]);assert.ok(Number.isSafeInteger(item.port)&&item.port>0&&item.port<65536);
    const expected=cancelBindingStateTemplate(kind,route,view,method,'http://127.0.0.1:'+item.port+'/body');
    equal({request1:item.request1,request2:item.request2},expected,'pending read/closed, detach, abort identities/event, late result, every retained byte/backing, both contexts/prototypes');
    for(const stage of ['afterRequest1','beforeRequest2','beforeStop','finalSources'])assert.equal(item[stage].length,2);
    equal(item.beforeStop.map(source=>source.path),['/body','/pending']);
    equal(item.events.map(event=>event.name),['request1-begun','request1-ended','continue-request2','request2-begun','request2-ended','finish']);
    const marks=Object.fromEntries(item.events.map(event=>[event.name,event.sequence])),sequences=[];
    for(const events of [item.events,...item.beforeStop.map(source=>source.events)]){let previous=-1;for(const event of events){fields(event,['name','sequence']);assert.ok(Number.isSafeInteger(event.sequence)&&event.sequence>previous);previous=event.sequence;sequences.push(previous);}}
    for(const key of ['request1SampleSequence','cancelSampleSequence','finalSampleSequence']){assert.ok(Number.isSafeInteger(item[key]));sequences.push(item[key]);}
    equal(sequences.sort((a,b)=>a-b),Array.from({length:16},(_,index)=>index));
    assert.ok(marks['request1-ended']<item.request1SampleSequence&&item.request1SampleSequence<item.cancelSampleSequence&&item.cancelSampleSequence<marks['continue-request2']&&marks['continue-request2']<marks['request2-begun']&&marks['request2-begun']<marks['request2-ended']&&marks['request2-ended']<item.finalSampleSequence&&item.finalSampleSequence<marks.finish);
    const packet=cancelBindingPacket(kind),split=packet.indexOf('\r\n\r\n')+4;
    for(const [index,path] of ['/body','/pending'].entries()){
        const last=item.beforeStop[index],request='GET '+path+' HTTP/1.1\r\nHost: 127.0.0.1:'+item.port;
        const source={path,request,rawRequest:Buffer.from(request+'\r\n\r\n').toString('base64'),peerEnded:false,peerReset:false,headersSent:true,bodySent:false,fixtureEnded:false,fixtureDestroyed:false,events:[],sentHeader:packet.subarray(0,split).toString('base64')};
        equal(last.events.map(event=>event.name),index===0?['request','headers-only','body-after-request2','peer-ended']:['request','headers-only','peer-ended']);assert.ok(last.events[1].sequence<marks['request1-ended']);
        if(index===0)assert.ok(marks['request2-begun']<last.events[2].sequence&&last.events[2].sequence<last.events[3].sequence&&last.events[3].sequence<item.finalSampleSequence);
        else assert.ok(last.events[2].sequence<item.cancelSampleSequence);
        for(const [stage,boundary] of [['afterRequest1',item.request1SampleSequence],['beforeRequest2',item.cancelSampleSequence],['beforeStop',item.finalSampleSequence],['finalSources',item.finalSampleSequence]]){
            const events=last.events.filter(event=>event.sequence<boundary),sent=index===0&&boundary===item.finalSampleSequence,ended=events.some(event=>event.name==='peer-ended');
            equal(item[stage][index],{...source,events,peerEnded:ended,bodySent:sent,fixtureEnded:sent,...(sent?{sent:packet.toString('base64')}:{})},'actual canceled source FIN before request2, unrelated source live until request2, complete wire bytes/FIN before stop');
        }
    }
    equal(item.child,{code:0,signal:null});equal(item.workerExit,{reaped:true,channelClosed:true,code:0});assert.equal(item.stderr,'');
    const lines=item.stdout.trim().split(/\r?\n/);assert.equal(lines.length,6);assert.match(lines[0],/^memoryLimit=0 actualFeatures=(\d+) expectedFeatures=\1$/);
    equal(lines.slice(1),['BINDING_REQUEST 1','BINDING_PHASE 1 '+JSON.stringify(item.request1),'BINDING_REQUEST 2','BINDING_PHASE 2 '+JSON.stringify(item.request2),'BINDING_EXIT '+JSON.stringify(item.workerExit)]);
    return expected;
}
export function validateCancelBindingReport(report,expected){
    fields(report,['schema','complete','runId','kind','route','inputs','binaries','cases']);assert.equal(report.schema,'capsid-binding-fetch-cancel-v1');assert.equal(report.complete,true);assert.equal(report.runId,expected.runId);assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);assert.equal(report.kind,expected.kind);assert.equal(report.route,expected.route);
    assert.ok(cancelBindingKinds.includes(report.kind)&&cancelBindingRoutes.includes(report.route));equal(report.inputs,expected.inputs);equal(report.binaries,expected.binaries);assert.equal(report.cases.length,26);
    return report.cases.map((item,index)=>assertCancelBindingCase(item,report.kind,report.route,cancelBindingViews[Math.floor(index/2)],cancelBindingMethods[index%2]));
}

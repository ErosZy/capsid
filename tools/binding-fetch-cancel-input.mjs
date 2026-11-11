import assert from 'node:assert/strict';
import {retainedBindingKinds,retainedBindingRoutes,retainedBindingViews,retainedBindingMethods,retainedBindingPacket,retainedBindingInputPaths,retainedBindingStateTemplate} from './binding-fetch-retained-input.mjs';
import {bindingArgumentCall,bindingArgumentWireTypes} from './binding-fetch-arguments-input.mjs';
export const cancelBindingKinds=retainedBindingKinds,cancelBindingRoutes=retainedBindingRoutes,cancelBindingViews=retainedBindingViews,cancelBindingMethods=retainedBindingMethods,cancelBindingPacket=retainedBindingPacket;
export const cancelBindingInputPaths=Object.freeze({...retainedBindingInputPaths,runner:'tests/test_binding_fetch_cancel.mjs',controls:'tests/test_binding_fetch_cancel_contract.mjs',binding:'tests/fixtures/binding-fetch-cancel-package.mjs',user:'tests/fixtures/binding-fetch-cancel-user.mjs',sourceModel:'tools/binding-fetch-cancel-input.mjs',contract:'tools/binding-fetch-cancel-contract.mjs',retainedModel:'tools/binding-fetch-retained-input.mjs'});
const models=new Map();
const freeze=value=>{if(value&&typeof value==='object'){for(const child of Object.values(value))freeze(child);Object.freeze(value);}return value;};
const wrap=data=>({data,types:bindingArgumentWireTypes(data)});
// Only fixed-input immutable expectations are shared. Every current report is
// compared again, including mutable reports previously accepted by a caller.
export function cancelBindingStateTemplate(kind,route,view,method,target){
    assert.ok(cancelBindingKinds.includes(kind)&&cancelBindingRoutes.includes(route)&&cancelBindingViews.includes(view)&&cancelBindingMethods.includes(method));assert.equal(typeof target,'string');
    const key=JSON.stringify([kind,route,view,method,target]);
    if(!models.has(key)){
        const pool=retainedBindingStateTemplate(kind,route,view,method,target),error={name:'AbortError',message:'The operation was aborted',code:20,isDOMException:true};
        const ready={ready:true,readState:'pending',closedState:'pending',inputAfter:0,pendingCall:bindingArgumentCall(),signalAborted:false,closedStable:true,call:bindingArgumentCall()};
        const inspected={finished:true,ready:true,readState:'rejected',closedState:'rejected',inputAfter:0,closedStable:true,signalAborted:true,signalReason:error,abortEvents:[{aborted:true,reason:error}],readError:error,closedError:error,readSameAsSignal:false,closedSameAsSignal:false,closedSameAsRead:true,bodyUsed:true,lockedBefore:true,pendingCall:bindingArgumentCall(),call:{...bindingArgumentCall(),requestId:'2'},lockedAfter:false};
        const model=freeze({request1:{opened:pool.request1,ready:wrap(ready),late:[]},request2:{inspected:wrap(inspected),reads:pool.request2.reads,closed:pool.request2.closed,late:[{status:'rejected',name:'Error',message:'binding call was canceled'}]}});
        if(models.size===32)models.delete(models.keys().next().value);models.set(key,model);
    }
    return models.get(key);
}

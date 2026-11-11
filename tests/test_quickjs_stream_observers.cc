// Stream observation scopes do not retain active jobs while parked.
// Unmarked promises keep the original request job policy.
#include "quickjs.h"
#include <cstdio>
#include <initializer_list>

namespace {
struct Scope {int id=0;bool live=true;int jobs=0;int origins=0;};
struct Harness {Scope* current=nullptr;int calls=0;int call_mask=0;};
int checks=0,failed=0;
void check(bool good,const char* message){++checks;if(!good){++failed;std::fprintf(stderr,"FAIL: %s\n",message);}}
int capture_job(JSContext*,void* opaque,void** out){auto& h=*static_cast<Harness*>(opaque);*out=h.current;if(h.current)++h.current->jobs;return 0;}
int capture_origin(JSContext*,void* opaque,void** out){auto& h=*static_cast<Harness*>(opaque);*out=h.current;if(h.current)++h.current->origins;return 0;}
void* enter(JSContext*,void* scope,void* opaque){auto& h=*static_cast<Harness*>(opaque);auto* previous=h.current;h.current=static_cast<Scope*>(scope);return previous;}
void leave(JSContext*,void* previous,void* opaque){static_cast<Harness*>(opaque)->current=static_cast<Scope*>(previous);}
void release_job(void* scope,void*){if(scope)--static_cast<Scope*>(scope)->jobs;}
void release_origin(void* scope,void*){if(scope)--static_cast<Scope*>(scope)->origins;}
bool can_run(JSContext*,void* scope,void*){return !scope||static_cast<Scope*>(scope)->live;}
bool same(JSContext*,void* origin,void* job,void* opaque){return origin==(job?job:static_cast<Harness*>(opaque)->current);}
JSValue observe(JSContext* ctx,JSValueConst,int argc,JSValueConst* argv){
    auto& h=*static_cast<Harness*>(JS_GetContextOpaque(ctx));++h.calls;if(h.current)h.call_mask|=1<<h.current->id;
    return argc?JS_DupValue(ctx,argv[0]):JS_UNDEFINED;
}
void drain(JSContext* ctx){auto* rt=JS_GetRuntime(ctx);unsigned rounds=0;
    while(JS_IsJobPending(rt)||JS_IsDeferredPromiseJobPending(rt)){
        check(++rounds<100,"bounded delivery drain");if(rounds>=100)break;
        JS_PromoteDeferredPromiseJobs(rt);while(JS_IsJobPending(rt)){JSContext* job_ctx=nullptr;check(JS_ExecutePendingJob(rt,&job_ctx)>=0,"job execution");}
    }
}
JSValue subscribe(JSContext* ctx,JSValueConst promise){
    JSValue callback=JS_NewCFunction(ctx,observe,"observe",1),args[2]={callback,callback};
    JSValue then=JS_GetPropertyStr(ctx,promise,"then"),result=JS_Call(ctx,then,promise,2,args);
    JS_FreeValue(ctx,then);JS_FreeValue(ctx,callback);check(!JS_IsException(result),"subscribe to native promise");return result;
}
void release_values(JSContext* ctx,JSValue source,JSValue child,JSValue* functions){
    JS_FreeValue(ctx,source);JS_FreeValue(ctx,child);JS_FreeValue(ctx,functions[0]);JS_FreeValue(ctx,functions[1]);JS_RunGC(JS_GetRuntime(ctx));
}
}
int main(){
    JSRuntime* rt=JS_NewRuntime();if(!rt)return 1;JSContext* ctx=JS_NewContext(rt);if(!ctx){JS_FreeRuntime(rt);return 1;}
    Harness h;Scope source{1},actor{2},second{3};JS_SetContextOpaque(ctx,&h);
    JSJobContextHooks jobs{capture_job,enter,leave,release_job};JS_SetJobContextHooks(rt,&jobs,&h);JS_SetJobContextCanRunHook(rt,can_run,&h);
    JSPromiseContextHooks origins{capture_origin,enter,leave,release_origin,can_run,same};JS_SetPromiseContextHooks(rt,&origins,&h);
    for(bool passive:{false,true})for(bool live:{false,true})for(bool reject:{false,true}){
        h.calls=h.call_mask=0;actor.live=live;h.current=&source;JSValue functions[2];JSValue promise=JS_NewPromiseCapability(ctx,functions);
        check(!JS_IsException(promise),"create native promise");if(passive)check(JS_MarkPromiseReactionsPassive(ctx,promise)==0,"mark passive observation");
        h.current=&actor;JSValue child=subscribe(ctx,promise);
        check(actor.jobs==(passive?0:2),"only parked stream observations avoid active job leases");
        check(actor.origins>0,"subscriber identity retained independently");h.current=&source;
        JSValue value=JS_NewInt32(ctx,73),settled=JS_Call(ctx,functions[reject?1:0],JS_UNDEFINED,1,&value);
        check(!JS_IsException(settled),"settle source promise");JS_FreeValue(ctx,settled);
        check(actor.jobs==((passive&&!live)?0:1),"live delivery acquires a bounded actual job lease");
        h.current=nullptr;drain(ctx);check(h.calls==(live?1:0),"retired observer cannot run; live observer runs once");
        check(h.call_mask==(live?(1<<actor.id):0),"callback runs in subscriber scope");
        check(actor.jobs==0&&source.jobs==0,"delivery releases every actual job lease");
        release_values(ctx,promise,child,functions);check(actor.origins==0&&source.origins==0,"observation metadata released after collection");
    }
    actor.live=true;
    for(bool live:{false,true}){
        second.live=live;h.calls=h.call_mask=0;h.current=&source;JSValue functions[2];JSValue promise=JS_NewPromiseCapability(ctx,functions);
        check(JS_MarkPromiseReactionsPassive(ctx,promise)==0,"mark source for derived observation");
        h.current=&actor;JSValue child=subscribe(ctx,promise);h.current=&second;JSValue grandchild=subscribe(ctx,child);
        check(actor.jobs==0&&second.jobs==0,"derived observation stays passive while parked");
        h.current=&source;JSValue value=JS_NewInt32(ctx,99),result=JS_Call(ctx,functions[0],JS_UNDEFINED,1,&value);JS_FreeValue(ctx,result);
        h.current=nullptr;drain(ctx);check(h.calls==(live?2:1),"derived live and retired scopes remain independent");
        check(h.call_mask==((1<<actor.id)|(live?(1<<second.id):0)),"derived callback scope identity");
        JS_FreeValue(ctx,grandchild);release_values(ctx,promise,child,functions);
        check(actor.jobs==0&&second.jobs==0&&source.jobs==0,"derived delivery releases every job lease");
        check(actor.origins==0&&second.origins==0&&source.origins==0,"derived metadata collected without retaining requests");
    }
    h.current=nullptr;check(JS_MarkPromiseReactionsPassive(ctx,JS_UNDEFINED)<0,"non-promise rejected");JS_FreeValue(ctx,JS_GetException(ctx));
    JS_FreeContext(ctx);JS_RunGC(rt);check(actor.jobs==0&&second.jobs==0&&source.jobs==0,"final active lease balance");
    check(actor.origins==0&&second.origins==0&&source.origins==0,"final inert identity balance");JS_FreeRuntime(rt);
    std::printf("{\"checks\":%d,\"failures\":%d}\n",checks,failed);return failed?1:0;
}

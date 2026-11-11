#include "quickjs.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
int checks=0,failures=0;
void check(bool value,const char* label){++checks;if(!value){++failures;std::fprintf(stderr,"FAIL: %s\n",label);}}
JSValue evaluate(JSContext* ctx,const std::string& text){
    JSValue value=JS_Eval(ctx,text.data(),text.size(),"value-output-regression.js",JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(value),"evaluate opcode/ownership program");
    if(JS_IsException(value)){JSValue error=JS_GetException(ctx);const char* message=JS_ToCString(ctx,error);
        std::fprintf(stderr,"evaluation error: %s\n",message?message:"unknown");JS_FreeCString(ctx,message);JS_FreeValue(ctx,error);}
    return value;
}
void number(JSContext* ctx,JSValueConst value,double expected){
    check(JS_IsNumber(value),"numeric result retains Number brand");
    if(!JS_IsNumber(value))return;
    double observed=0;check(JS_ToFloat64(ctx,&observed,value)==0,"read raw numeric result");
    check(std::isnan(expected)?std::isnan(observed):observed==expected,"independent native numeric expectation");
    if(expected==0)check(std::signbit(observed)==std::signbit(expected),"signed zero preserved");
}
void numbers(JSContext* ctx,const std::string& source,const std::vector<double>& expected){
    JSValue array=evaluate(ctx,source);if(JS_IsException(array))return;
    check(JS_IsArray(array)==1,"numeric program returns an array");JSValue length=JS_GetPropertyStr(ctx,array,"length");uint32_t count=0;
    check(JS_ToUint32(ctx,&count,length)==0&&count==expected.size(),"complete numeric result inventory");JS_FreeValue(ctx,length);
    for(uint32_t index=0;index<expected.size();++index){JSValue item=JS_GetPropertyUint32(ctx,array,index);number(ctx,item,expected[index]);JS_FreeValue(ctx,item);}
    JS_FreeValue(ctx,array);
}
void booleans(JSContext* ctx,const std::string& source,const std::vector<bool>& expected){
    JSValue array=evaluate(ctx,source);if(JS_IsException(array))return;
    JSValue length=JS_GetPropertyStr(ctx,array,"length");uint32_t count=0;
    check(JS_IsArray(array)==1&&JS_ToUint32(ctx,&count,length)==0&&count==expected.size(),"complete boolean result inventory");JS_FreeValue(ctx,length);
    for(uint32_t index=0;index<expected.size();++index){JSValue item=JS_GetPropertyUint32(ctx,array,index);
        check(JS_IsBool(item),"comparison and conversion retain Boolean brand");check(JS_ToBool(ctx,item)==(expected[index]?1:0),"independent native boolean expectation");JS_FreeValue(ctx,item);}
    JS_FreeValue(ctx,array);
}
void release_buffer(JSRuntime*,void* opaque,void*){++*static_cast<int*>(opaque);}
}
int main(){
    JSRuntime* rt=JS_NewRuntime();if(!rt)return 1;JSContext* ctx=JS_NewContext(rt);if(!ctx){JS_FreeRuntime(rt);return 1;}
    const double inf=std::numeric_limits<double>::infinity(),nan=std::numeric_limits<double>::quiet_NaN();
    struct Numeric{double value;int32_t signed_value;};
    const std::vector<Numeric> input={{0,0},{-0.0,0},{1,1},{-1,-1},{7,7},{8,8},{127,127},{128,128},
        {32767,32767},{32768,32768},{2147483647.0,2147483647},{2147483648.0,INT32_MIN},
        {4294967295.0,-1},{4294967296.0,0},{-2147483649.0,INT32_MAX},{-4294967297.0,-1},
        {3.5,3},{-3.5,-3},{nan,0},{inf,0},{-inf,0}};
    JSValue global=JS_GetGlobalObject(ctx),array=JS_NewArray(ctx);
    for(uint32_t index=0;index<input.size();++index)check(JS_SetPropertyUint32(ctx,array,index,JS_NewFloat64(ctx,input[index].value))>=0,"inject independent native numeric input");
    check(JS_SetPropertyStr(ctx,global,"__numbers",array)>=0,"install independent numeric inputs");JS_FreeValue(ctx,global);
    std::vector<double> expected;std::vector<bool> truth;
    for(const auto& item:input){
        expected.push_back(item.signed_value);expected.push_back(static_cast<uint32_t>(item.signed_value));
        expected.push_back(item.value+0.5);expected.push_back(item.value-0.5);expected.push_back(item.value*1.5);
        expected.push_back(item.value/2);expected.push_back(-item.value);
        truth.push_back(item.value==0);truth.push_back(item.value<0);truth.push_back(!std::isnan(item.value));truth.push_back(item.value!=0&&!std::isnan(item.value));truth.push_back(item.value==0||std::isnan(item.value));
    }
    numbers(ctx,"[].concat(...__numbers.map(x=>[x|0,x>>>0,x+0.5,x-0.5,x*1.5,x/2,-x]))",expected);
    booleans(ctx,"[].concat(...__numbers.map(x=>[x===0,x<0,x===x,!!x,!x]))",truth);
    numbers(ctx,"[-2147483648,-32769,-32768,-129,-128,-1,0,1,7,8,127,128,32767,32768,2147483647,2147483648,4294967295,-0,0.5,-0.5]",
        {-2147483648.0,-32769,-32768,-129,-128,-1,0,1,7,8,127,128,32767,32768,2147483647.0,2147483648.0,4294967295.0,-0.0,0.5,-0.5});
    global=JS_GetGlobalObject(ctx);
    check(JS_SetPropertyStr(ctx,global,"__yesDelete",JS_NewInt32(ctx,1))>=0,"install configurable global");
    check(JS_DefinePropertyValueStr(ctx,global,"__noDelete",JS_NewInt32(ctx,1),JS_PROP_WRITABLE|JS_PROP_ENUMERABLE)>=0,"install nonconfigurable global");
    JS_FreeValue(ctx,global);
    booleans(ctx,"[delete __yesDelete,delete __noDelete]",{true,false});
    booleans(ctx,R"JS((()=>{
        function remove(o){with(o){return delete field;}}
        return [remove({field:1}),remove(Object.defineProperty({},'field',{value:1}))];
    })())JS",{true,false});
    numbers(ctx,R"JS((()=>{
        function* inner(){yield 1;return 2;}function* outer(){return yield* inner();}
        const g=outer(),a=g.next(),b=g.next();return [a.value,b.value];
    })())JS",{1,2});
    booleans(ctx,R"JS((()=>{
        function* inner(){yield 1;return 2;}function* outer(){return yield* inner();}
        const g=outer(),a=g.next(),b=g.next();return [a.done,b.done];
    })())JS",{false,true});
    numbers(ctx,R"JS((()=>{
        const result=[];
        for(const hasReturn of [true,false]){
            const iterator={next(){return {value:10,done:false};},[Symbol.iterator](){return this;},return(){return {value:20,done:true};}};
            if(!hasReturn)delete iterator.return;
            function* delegate(){return yield* iterator;}const g=delegate(),a=g.next(),b=g.return(99);
            result.push(a.value,b.value);
        }
        return result;
    })())JS",{10,20,10,99});
    JSValue text=evaluate(ctx,"(()=>{let s='capsid-\\u{1f4e6}';const id=x=>x;return id(s);})()");
    const char* utf8=JS_ToCString(ctx,text);check(utf8&&std::string(utf8)=="capsid-\xf0\x9f\x93\xa6","constant/local/argument string copies preserve UTF-8");JS_FreeCString(ctx,utf8);JS_FreeValue(ctx,text);

    JSValue marker=JS_NewObject(ctx);
    check(JS_IsObject(marker),"create independent native exception marker");
    global=JS_GetGlobalObject(ctx);
    check(JS_SetPropertyStr(ctx,global,"__exceptionMarker",JS_DupValue(ctx,marker))>=0,"install native exception identity");
    JS_FreeValue(ctx,global);
    const std::vector<std::string> throwing={
        "(()=>{throw __exceptionMarker;})()",
        "({get field(){throw __exceptionMarker;}}).field",
        "new Proxy({}, {get(){throw __exceptionMarker;}}).field",
        "+{[Symbol.toPrimitive](){throw __exceptionMarker;}}",
        "`${{toString(){throw __exceptionMarker;}}}`",
        "(()=>{const o={set field(v){throw __exceptionMarker;}};o.field=1;})()",
        "(()=>{const o=new Proxy({}, {set(){throw __exceptionMarker;}});o.field=1;})()",
        "(()=>{const o=new Proxy({}, {deleteProperty(){throw __exceptionMarker;}});return delete o.field;})()",
        "(()=>{const o=new Proxy({}, {has(){throw __exceptionMarker;}});return 'field' in o;})()",
        "(()=>{const o=new Proxy({}, {ownKeys(){throw __exceptionMarker;}});return {...o};})()",
        "(()=>{const o={ [Symbol.iterator](){return {next(){throw __exceptionMarker;}};}};return [...o];})()",
        "(()=>{const {field}=({get field(){throw __exceptionMarker;}});return field;})()",
        "new (class {constructor(){throw __exceptionMarker;}})()",
        "(()=>{function* inner(){throw __exceptionMarker;}function* outer(){yield* inner();}return outer().next();})()"
    };
    for(const auto& program:throwing){
        JSValue result=JS_Eval(ctx,program.data(),program.size(),"value-exception-regression.js",JS_EVAL_TYPE_GLOBAL);
        check(JS_IsException(result),"throwing opcode reports the exception sentinel to native caller");
        if(JS_IsException(result)){
            JSValue observed=JS_GetException(ctx);
            check(JS_IsObject(observed)&&JS_VALUE_GET_PTR(observed)==JS_VALUE_GET_PTR(marker),"exception preserves raw native marker identity");
            JS_FreeValue(ctx,observed);
        }
        JS_FreeValue(ctx,result);
        JSValue recovered=evaluate(ctx,"6*7");number(ctx,recovered,42);JS_FreeValue(ctx,recovered);
    }
    global=JS_GetGlobalObject(ctx);
    check(JS_SetPropertyStr(ctx,global,"__exceptionMarker",JS_UNDEFINED)>=0,"retire exception marker global");
    JS_FreeValue(ctx,global);JS_FreeValue(ctx,marker);

    int freed=0;std::array<uint8_t,32> data{};for(size_t index=0;index<data.size();++index)data[index]=static_cast<uint8_t>(index);
    global=JS_GetGlobalObject(ctx);JSValue buffer=JS_NewArrayBuffer(ctx,data.data(),data.size(),release_buffer,&freed,false);
    check(!JS_IsException(buffer),"create externally owned buffer");check(JS_SetPropertyStr(ctx,global,"__valueBuffer",buffer)>=0,"transfer external owner into realm");JS_FreeValue(ctx,global);
    booleans(ctx,R"JS((()=>{
        const b=globalThis.__valueBuffer,id=x=>x;
        const obj={value:b,get selected(){return b;}};let first=b,second=first;[first,second]=[second,first];
        const closure=()=>b;function normalThis(){return this;}function strictThis(){'use strict';return this;}
        class Box{constructor(value){this.value=value;}}
        globalThis.__valueKeep=[b,id(b),obj,closure,new Map([[b,b]]),new Box(b)];
        delete globalThis.__valueBuffer;
        return [first===b,second===b,id(b)===b,obj.selected===b,closure()===b,normalThis.call(obj)===obj,
            strictThis.call(b)===b,globalThis.__valueKeep[4].get(b)===b,globalThis.__valueKeep[5].value===b,
            new Uint8Array(b).byteLength===32];
    })())JS",std::vector<bool>(10,true));
    std::string wide="(()=>{const b=globalThis.__valueKeep[0];function wide(";
    for(unsigned index=0;index<260;++index){if(index)wide+=',';wide+='p'+std::to_string(index);}
    wide+=",extra=b){";
    for(unsigned index=0;index<260;++index)wide+="let v"+std::to_string(index)+"=p"+std::to_string(index)+";";
    wide+="return [v0===b,v127===b,v255===b,v259===b,extra===b,(()=>v257)()===b";
    for(unsigned index=0;index<260;++index)wide+=",v"+std::to_string(index)+"===b";
    wide+="];}return wide(";
    for(unsigned index=0;index<260;++index){if(index)wide+=',';wide+='b';}wide+=");})()";
    booleans(ctx,wide,std::vector<bool>(266,true));
    global=JS_GetGlobalObject(ctx);array=JS_NewArray(ctx);
    for(uint32_t index=0;index<260;++index)check(JS_SetPropertyUint32(ctx,array,index,JS_NewInt32(ctx,static_cast<int32_t>(index)*13-1000))>=0,"inject distinct wide argument values");
    check(JS_SetPropertyStr(ctx,global,"__wideNumbers",array)>=0,"install wide argument inventory");JS_FreeValue(ctx,global);
    std::string numeric_wide="(()=>{function wide(";
    for(unsigned index=0;index<260;++index){if(index)numeric_wide+=',';numeric_wide+='p'+std::to_string(index);}
    numeric_wide+=",extra=1234567){";
    for(unsigned index=0;index<260;++index)numeric_wide+="let v"+std::to_string(index)+"=p"+std::to_string(index)+";";
    numeric_wide+="return [v0,v127,v255,v259,extra,(()=>v257)()";
    for(unsigned index=0;index<260;++index)numeric_wide+=",v"+std::to_string(index);
    numeric_wide+="];}return wide(...globalThis.__wideNumbers);})()";
    std::vector<double> expected_wide={-1000,651,2315,2367,1234567,2341};
    for(unsigned index=0;index<260;++index)expected_wide.push_back(static_cast<int32_t>(index)*13-1000);
    numbers(ctx,numeric_wide,expected_wide);
    JS_RunGC(rt);check(freed==0,"copied buffers remain alive through closure/property/map/wide argument roots");
    JSValue mutation=evaluate(ctx,"new Uint8Array(globalThis.__valueKeep[0])[0]=123;0");JS_FreeValue(ctx,mutation);check(data[0]==123,"retained objects still share actual external bytes");
    JSValue dropped=evaluate(ctx,"globalThis.__valueKeep[0];globalThis.__valueKeep[1];0");JS_FreeValue(ctx,dropped);
    JSValue cleared=evaluate(ctx,"delete globalThis.__valueKeep;0");JS_FreeValue(ctx,cleared);JS_RunGC(rt);check(freed==1,"external owner released exactly once after all copies retire");
    // A failed owner invariant is already an explicit test failure. Return
    // nonzero before asking a deliberately broken fault seed to tear down a
    // graph with invalid reference counts; successful runs still free all.
    if(freed!=1){std::printf("{\"checks\":%d,\"failures\":%d}\n",checks,failures);return 1;}
    JS_FreeContext(ctx);JS_RunGC(rt);JS_FreeRuntime(rt);check(freed==1,"final collection cannot free the owner twice");
    std::printf("{\"checks\":%d,\"failures\":%d}\n",checks,failures);return failures?1:0;
}

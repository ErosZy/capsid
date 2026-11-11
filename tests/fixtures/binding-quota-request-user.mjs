import echo from 'capsid:binding/echo';
let pending=[],late=[];
const wrap=data=>{const types={objects:0,arrays:0,objectNullPrototypes:true,arrayPrototypes:true};const visit=value=>{if(!value||typeof value!=='object')return;if(Array.isArray(value)){types.arrays++;types.arrayPrototypes&&=Object.getPrototypeOf(value)===Array.prototype;}else{types.objects++;types.objectNullPrototypes&&=Object.getPrototypeOf(value)===null;}for(const child of Object.values(value))visit(child);};visit(data);return {data,types};};
export default {async fetch(request){
    const url=new URL(request.url);
    if(url.searchParams.get('phase')==='open'){
        for(let id=0;id<63;id++)pending.push(echo.hold({id}).then(value=>late.push({id,status:'fulfilled',value}),error=>late.push({id,status:'rejected',name:error.name,message:error.message})));
        const barrier=echo.barrier({}),variant=url.searchParams.get('views');let observed=0,input={id:63};if(variant==='symbol')input=Symbol('overflow');if(variant==='cycle')input.self=input;if(variant==='getter')Object.defineProperty(input,'extra',{enumerable:true,get(){observed++;return 1;}});
        let overflow,phase='synchronous';try{const promise=echo.hold(input);phase='promise';overflow=await promise.then(value=>({accepted:true,value}),error=>({accepted:false,name:error.name,message:error.message}));}catch(error){overflow={accepted:false,name:error.name,message:error.message};}
        const before=wrap(await barrier),recovered=wrap(await echo.ping({value:'after64th-completed'}));
        return Response.json({variant,overflowPhase:phase,overflow,observed,before,recovered,lateBeforeEnd:[...late]});
    }
    await Promise.all(pending);const stats=wrap(await echo.stats({}));return Response.json({stats,late:[...late]});
}};

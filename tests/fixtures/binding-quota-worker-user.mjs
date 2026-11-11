import echo from 'capsid:binding/echo';
const late=[],pending=[],byParent=new Map();let recoveryBefore;
const wrap=data=>{const types={objects:0,arrays:0,objectNullPrototypes:true,arrayPrototypes:true};const visit=value=>{if(!value||typeof value!=='object')return;if(Array.isArray(value)){types.arrays++;types.arrayPrototypes&&=Object.getPrototypeOf(value)===Array.prototype;}else{types.objects++;types.objectNullPrototypes&&=Object.getPrototypeOf(value)===null;}for(const child of Object.values(value))visit(child);};visit(data);return {data,types};};
export default {async fetch(request){
    const url=new URL(request.url),step=url.searchParams.get('step'),variant=url.searchParams.get('variant'),parent=url.searchParams.get('parent');
    if(step==='hold'){
        if(variant==='recovery'){await Promise.all(byParent.get('1'));const summary=wrap(await echo.summary({}));recoveryBefore={summary,late:[...late]};}
        const local=[];for(let slot=0;slot<64;slot++)local.push(echo.hold({slot}).then(value=>late.push({parent,slot,status:'fulfilled',value}),error=>late.push({parent,slot,status:'rejected',name:error.name,message:error.message})));
        pending.push(...local);byParent.set(parent,local);await new Promise(()=>{});throw new Error('held parent must stay pending');
    }
    if(step==='overflow'){
        let observed=0,input={slot:64};if(variant==='symbol')input=Symbol('overflow');if(variant==='cycle')input.self=input;if(variant==='getter')Object.defineProperty(input,'extra',{enumerable:true,get(){observed++;return 1;}});
        let phase='synchronous',result;try{const value=echo.hold(input);phase='promise';result=await value.then(value=>({accepted:true,value}),error=>({accepted:false,name:error.name,message:error.message}));}catch(error){result={accepted:false,name:error.name,message:error.message};}return Response.json({variant,phase,result,observed});
    }
    await Promise.all(pending);
    const summary=wrap(await echo.summary({})),slices=[];for(const request of [...Array.from({length:16},(_,index)=>String(index+1)),'21'])slices.push(wrap(await echo.slice({request})));
    return Response.json({summary,slices,late:[...late],recoveryBefore});
}};

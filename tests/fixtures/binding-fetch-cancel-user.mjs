import echo from 'capsid:binding/echo';
const late=[];
const types=value=>{let objects=0,arrays=0,objectNullPrototypes=true,arrayPrototypes=true;const visit=item=>{if(!item||typeof item!=='object')return;if(Array.isArray(item)){arrays++;arrayPrototypes&&=Object.getPrototypeOf(item)===Array.prototype;}else{objects++;objectNullPrototypes&&=Object.getPrototypeOf(item)===null;}for(const child of Object.values(item))visit(child);};visit(value);return {objects,arrays,objectNullPrototypes,arrayPrototypes};};
const wrap=data=>({data,types:types(data)});
export default {async fetch(request){
    const url=new URL(request.url),[route,view,method]=url.searchParams.get('views').split(','),target=url.searchParams.get('target');
    if(url.searchParams.get('phase')==='open'){
        const opened=wrap(await echo.open({target,route,view,method}));
        echo.wait({target:target.replace(/\/body$/,'/pending'),route,view,method}).then(value=>late.push({status:'fulfilled',value}),reason=>late.push({status:'rejected',name:reason.name,message:reason.message}));
        const ready=wrap(await echo.ready({}));return Response.json({opened,ready,late:[...late]});
    }
    const inspected=wrap(await echo.inspect({})),reads=[];
    for(let index=0;index<10;index++){const data=await echo.read({index});reads.push(wrap(data));if(data.read.done)break;if(index===9)throw new Error('bounded retained body EOF');}
    const closed=wrap(await echo.close({}));return Response.json({inspected,reads,closed,late:[...late]});
}};

import echo from 'capsid:binding/echo';
const types=value=>{let objects=0,arrays=0,objectNullPrototypes=true,arrayPrototypes=true;const visit=item=>{if(!item||typeof item!=='object')return;
    if(Array.isArray(item)){arrays++;arrayPrototypes&&=Object.getPrototypeOf(item)===Array.prototype;}else{objects++;objectNullPrototypes&&=Object.getPrototypeOf(item)===null;}for(const child of Object.values(item))visit(child);};visit(value);return {objects,arrays,objectNullPrototypes,arrayPrototypes};};
export default {async fetch(request){
    const url=new URL(request.url),[route,view,method]=url.searchParams.get('views').split(',');
    if(url.searchParams.get('phase')==='open'){const data=await echo.open({target:url.searchParams.get('target'),route,view,method});return Response.json({data,types:types(data)});}
    const reads=[];for(let index=0;index<10;index++){const data=await echo.read({index});reads.push({data,types:types(data)});if(data.read.done)break;if(index===9)throw new Error('bounded retained body EOF');}
    const data=await echo.close({});return Response.json({reads,closed:{data,types:types(data)}});
}};

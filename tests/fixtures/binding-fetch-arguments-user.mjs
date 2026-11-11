import echo from 'capsid:binding/echo';
const validationScenarios=['capacity','capacity-first-count','zero','negative','signed-max','signed-min','range-high','range-low',
    'range-fraction','string-capacity','object-capacity','primitive-capacity','bigint','symbol','object-bigint','object-symbol',
    'zero-null','zero-nan','zero-infinity','zero-fraction','conversion-error','primitive-error','primitive-noncallable',
    'released-capacity','released-zero','released-object-capacity','released-conversion-error','release-during-conversion',
    'empty-capacity','empty-zero','detached-capacity','detach-during-conversion','invalid-view','invalid-view-conversion-error',
    'wrong-receiver','proxy-receiver','valid-default','valid-minimum','valid-fraction','valid-string'];
const inspect=value=>{
    let objects=0,arrays=0,objectNullPrototypes=true,arrayPrototypes=true;
    const visit=item=>{if(!item||typeof item!=='object')return;
        if(Array.isArray(item)){arrays++;arrayPrototypes&&=Object.getPrototypeOf(item)===Array.prototype;}
        else{objects++;objectNullPrototypes&&=Object.getPrototypeOf(item)===null;}
        for(const value of Object.values(item))visit(value);
    };visit(value);return {objects,arrays,objectNullPrototypes,arrayPrototypes};
};
export default {async fetch(request){
    const url=new URL(request.url),[route,view,method]=url.searchParams.get('views').split(','),kind='native-arguments-'+route+'-'+view+'-'+method;
    const state={kind,challenge:'binding-arguments',rows:[],unexpected:null},calls=[],wireTypes=[];
    const scenarios=[...validationScenarios,...(method==='read'?['options-getter-error','options-null','options-primitive','getter-detaches-then-throws']:[])];
    for(const scenario of scenarios){
        const envelope=await echo.argument({route,view,method,scenario,kind,challenge:state.challenge,target:url.searchParams.get('target')});
        wireTypes.push(inspect(envelope));calls.push(envelope.call);
        if(envelope.state.unexpected!==null){state.unexpected=envelope.state.unexpected;break;}
        if(envelope.state.kind!==kind||envelope.state.challenge!==state.challenge||envelope.state.rows.length!==1)throw new Error('incorrect single-row argument RPC');
        state.rows.push(envelope.state.rows[0]);
    }
    return Response.json({state,calls,wireTypes});
}};

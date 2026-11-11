from pathlib import Path
import json,sys

# Independent fixed-input model; never read runtime/reference reports.
INVOCATION='Illegal invocation: function called with incorrect `this` reference. See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.'
CONSTRUCTOR="Failed to construct 'Headers': constructor parameter 1 is not of type 'Headers or Sequence or object'."
SEQUENCE="Incorrect type: the provided value is not of type 'Sequence'."
PAIR='To initialize a Headers object from a sequence, each inner sequence must have exactly two elements.'
CALLBACK="Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'function'."
def exact(a,b):
    assert type(a)is type(b),(type(a),type(b))
    if isinstance(b,dict):
        assert set(a)==set(b),(set(a)^set(b))
        for key in b:exact(a[key],b[key])
    elif isinstance(b,list):
        assert len(a)==len(b),(len(a),len(b))
        for left,right in zip(a,b):exact(left,right)
    else:assert a==b,(a,b)
def key_name(name):
    raw=name.encode('utf-8');mask=(1<<64)-1;out=bytearray()
    for offset in range(0,len(raw),8):
        block=raw[offset:offset+8];word=int.from_bytes(block,'little');word^=((((word+0x3f3f3f3f3f3f3f3f)&mask)^((word+0x2525252525252525)&mask))&0x8080808080808080)>>2;out.extend(word.to_bytes(8,'little')[:len(block)])
    return bytes(out),bytes(out).decode('utf-8',errors='replace')
def snapshot(pairs=()):
    groups={};names={}
    for name,value in pairs:
        raw,display=key_name(name);names[raw]=display;groups.setdefault(raw,[]).append(value.strip('\t\r\n '))
    entries=[];cookies=[];get_cookie=None
    for raw in sorted(groups):
        values=groups[raw];name=names[raw]
        if raw==b'set-cookie':entries.extend([[name,v] for v in values]);cookies=values[:];get_cookie=', '.join(values)
        else:entries.append([name,', '.join(values)])
    return {'entries':entries,'keys':[row[0] for row in entries],'values':[row[1] for row in entries],'cookies':cookies,'getCookie':get_cookie,'tag':'[object Headers]'}
def expected_rows():
    rows=[]
    def add(id,value=None,trace=(),error=None):rows.append({'id':id,'value':value,'error':error,'trace':list(trace)})
    def fail(id,message,trace=(),sentinel=False):add(id,None,trace,{'name':'Error' if sentinel else 'TypeError','message':message,'code':None,'sameSentinel':sentinel})
    def thrown(id,trace):fail(id,'header-sentinel',trace,True)
    add('constructor-empty',snapshot())
    for kind in ['null','false','zero','string','bigint','symbol']:fail('constructor-'+kind,CONSTRUCTOR)
    add('record-own-enumerable',snapshot([('C','3'),('a','1'),('B','2')]))
    add('record-getter-order',snapshot([('C','C-value'),('a','a-value'),('B','B-value')]),['C','a','B'])
    add('record-proxy-order',snapshot([('C','3'),('a','1')]),['get:Symbol(Symbol.iterator)','keys','descriptor:C','descriptor:a','get:C','get:a'])
    add('sequence-duplicates',snapshot([('Z',' z '),('A',' 1\t'),('a','2'),('set-cookie','a=1'),('Set-Cookie','b=2')]))
    add('sequence-outer-iterator',snapshot([('X','1'),('Y','2')]),['outer-get','outer-run'])
    add('sequence-inner-iterator',snapshot([('X','1')]),['inner-get','inner-run'])
    for kind in ['null','one','object']:add('outer-iterator-'+kind,snapshot([('X','1')]))
    for kind in ['null','number','string']:fail('pair-'+kind,SEQUENCE)
    for kind in ['short','long']:fail('pair-'+kind,PAIR)
    fail('pair-arraylike',SEQUENCE)
    thrown('pair-conversion-order',['name','value']);thrown('record-getter-throw',['A'])
    for kind in ['copy-own-iterator','copy-own-iterator-getter']:add(kind,snapshot([('a','1')]))
    add('copy-own-getSetCookie',snapshot([('set-cookie','a=1'),('set-cookie','b=2')]))
    add('copy-subclass-iterator',snapshot([('a','1')]))
    for method in ['append','set']:
        thrown(method+'-conversion-order',['name','value']);add(method+'-primitive-order',snapshot([('x',' y ')]),['name:string','value:string'])
        for count in [0,1]:fail(method+'-arity-'+str(count),"Failed to execute '"+method+"' on 'Headers': parameter "+str(count+1)+" is not of type 'string'.")
    add('cookie-defensive-copy',snapshot([('set-cookie','a=1'),('set-cookie','b=2')]))
    add('cookie-set-delete-append',{'first':snapshot([('set-cookie','c=3')]),'last':snapshot([('set-cookie','d=4')])})
    fail('getSetCookie-wrong-receiver',INVOCATION)
    for kind in ['missing','null','object']:fail('forEach-'+kind,CALLBACK)
    for kind in ['forEach-wrong-receiver','forEach-wrong-receiver-null-callback']:fail(kind,INVOCATION)
    add('forEach-arguments-this',[{'value':v,'key':k,'parentSame':True,'thisSame':True,'argc':3} for k,v in [('a','1'),('b','2'),('set-cookie','x=1'),('set-cookie','y=2')]])
    for kind in ['forEach-own-entries','forEach-callback-own-call']:add(kind,snapshot([('a','1')]),['a=1'])
    add('forEach-live-mutation',snapshot([('a','1'),('b','2'),('d','4')]),['a=1','c=3'])
    for method,label in [('entries','EntryIterator'),('keys','KeyIterator'),('values','ValueIterator')]:
        initial=[['a','1'],['c','3']] if method=='entries' else ['a','c'] if method=='keys' else ['1','3']
        add(method+'-mutation-before-next',{'items':initial,'final':snapshot([('c','changed'),('b','2')]),'tag':'[object '+label+']','self':True})
        add(method+'-mutation-after-next',{'first':{'done':False,'value':initial[0]},'rest':initial[1:],'final':snapshot([('a','1'),('b','2')])})
        fail(method+'-wrong-receiver',INVOCATION)
    fail('iterator-next-wrong-receiver',INVOCATION)
    add('iterator-after-done',{'first':{'done':False,'value':['a','1']},'done':{'done':True,'value':None},'after':{'done':True,'value':None}})
    add('constructor-empty-name',snapshot([('','1')]))
    add('constructor-unicode-name',snapshot([('x-中文','1')]))
    add('constructor-unicode-value',snapshot([('x','中文')]))
    fail('sequence-convert-all-before-validate','Invalid header name.',['name1','value1','name2','value2'])
    thrown('sequence-convert-later-throw-before-validation',['later-value'])
    fail('sequence-generator-close','Invalid header name.',['first','second','finally'])
    fail('record-convert-all-before-validate','Invalid header name.',['first-value','second-get','second-value'])
    fail('pair-short-coercion',PAIR,['short-name']);fail('pair-long-coercion',PAIR,['name','value','extra']);thrown('pair-long-later-throw',['extra'])
    for kind in ['null','number']:fail('inner-iterator-'+kind,SEQUENCE)
    add('outer-array-own-iterator',snapshot([('b','2')]),['array-iterator']);thrown('outer-array-own-iterator-getter',['array-get'])
    add('inner-array-own-iterator',snapshot([('b','2')]),['pair-iterator']);thrown('inner-array-own-iterator-getter',['pair-get'])
    add('sparse-inner-array',snapshot([('undefined','undefined')]))
    for left in ['entries','keys','values']:
        for right in ['entries','keys','values']:
            if left!=right:fail('iterator-'+left+'-next-on-'+right,INVOCATION)
    add('iterator-kind-prototypes',{'entryKeyPrototypeSame':False,'keyValuePrototypeSame':False,'entryKeyNextSame':False,'keyValueNextSame':False,'tags':['[object EntryIterator]','[object KeyIterator]','[object ValueIterator]']})
    thrown('forEach-callback-throw',['a=1']);add('forEach-primitive-this',{'type':'number','value':7,'key':'a','headerValue':'1'})
    add('outer-cached-next',snapshot([('x','1')]),['iterator-call','next-get','next-call','next-call'])
    thrown('inner-coercion-iterator-close',['inner-first','inner-second','convert','inner-finally'])
    thrown('outer-coercion-iterator-close',['outer-first','convert','outer-finally'])
    thrown('outer-next-throw-close',['next']);thrown('pair-short-later-conversion-error',['later'])
    for kind in ['constructor-invalid-name-and-value','constructor-invalid-record-name-and-value','append-invalid-name-and-value']:fail(kind,'Invalid header value.')
    add('iterator-method-metadata',[{'nextName':'next','nextLength':0,'iteratorName':'','iteratorLength':0,'iteratorOwn':True,'returnType':'undefined'} for unused in range(3)])
    fail('iterator-symbol-generic-receiver',INVOCATION)
    add('iterator-result-properties',{'firstKeys':['done','value'],'lastKeys':['done','value'],'firstValueOwn':True,'lastValueOwn':True})
    add('constructor-subclass-append',snapshot([('x','1')]))
    fail('constructor-proxy-Headers',INVOCATION);add('copy-subclass-forEach',snapshot([('x','1')]))
    assert len(rows)==len({row['id'] for row in rows})==99
    return rows

def encoded_model():
    data={'schema':'capsid-headers-expected-inputs-v1','functionSHA256':'32ac93d22c97832d1b6c592d3e85f45af3c288b9c12aa6944b4368eb7898c87c','caseCount':99,'cases':expected_rows()}
    return (json.dumps(data,ensure_ascii=False,indent=2)+'\n').encode('utf-8')

if __name__=='__main__':
    destination=Path(__file__).resolve().parents[1]/'tests/fixtures/headers-expected-input-model.json'
    assert sys.argv[1:]==['--check'],'usage: headers-input-model.py --check'
    assert destination.read_bytes()==encoded_model(),'fixed expected data differs from independent source model'
    print(json.dumps({'complete':True,'cases':99,'sourceModelMatchesArtifact':True}))

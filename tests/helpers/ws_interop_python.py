"""Independent websockets client and protocol server for the T13 matrix."""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import platform
import ssl
import sys
from urllib.parse import parse_qs,urlsplit

import websockets
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosedOK

ROOT=Path(__file__).resolve().parents[2]
LOCK=json.loads((ROOT/'tests/fixtures/websocket-interop-lock.json').read_text())

def require(value,message):
    if not value:
        raise AssertionError(message)

def identity():
    package=Path(websockets.__file__).resolve().parent
    files=sorted([[p.relative_to(package).as_posix(),hashlib.sha256(p.read_bytes()).hexdigest()]
                  for p in package.rglob('*.py')])
    require(not list(package.rglob('*.so')) and not list(package.rglob('*.pyd')),'pure Python peer required')
    actual={'implementation':platform.python_implementation(),'python':platform.python_version(),
            'websockets':websockets.__version__,'sourceFiles':len(files),
            'sourceSHA256':hashlib.sha256(json.dumps(files,separators=(',',':')).encode()).hexdigest()}
    require(actual['implementation']==LOCK['python']['implementation'] and actual['python']==LOCK['python']['version'],'locked Python runtime')
    require(actual['websockets']==LOCK['websockets']['version'] and actual['sourceFiles']==LOCK['websockets']['sourceFiles'] and
            actual['sourceSHA256']==LOCK['websockets']['sourceSHA256'],'locked Python WebSocket implementation')
    return actual

def emit(value):
    print(json.dumps(value,separators=(',',':'),ensure_ascii=True),flush=True)

def summary(data):
    raw=data.encode() if isinstance(data,str) else bytes(data)
    return {'type':'text' if isinstance(data,str) else 'binary','length':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}

def payloads(challenge):
    return ['challenge:'+challenge,'hello\0界😀',b'',bytes((i*17+29)%251 for i in range(180000))]

def clean_close(connection,code,reason):
    p=connection.protocol
    require(p.close_rcvd is not None and p.close_sent is not None,'both actual Close frames')
    require(p.close_rcvd.code==code and p.close_sent.code==code and p.close_rcvd.reason==reason and p.close_sent.reason==reason,'exact Close code/reason')
    return {'code':connection.close_code,'reason':connection.close_reason,'clean':True}

async def client(args):
    info=identity()
    tls=ssl.create_default_context(cafile=args.ca) if args.uri.startswith('wss:') else None
    options={'subprotocols':['chat','super'],'compression':None,'proxy':None,'ping_interval':None,
             'open_timeout':4,'close_timeout':2,'max_size':1024*1024}
    if tls is not None:options['ssl']=tls
    result={'identity':info,'uri':args.uri,'case':args.case,'messages':[],'pingPong':0}
    if args.operation=='tls-reject':
        try:
            async with connect(args.uri,**options):
                raise AssertionError('invalid TLS fixture unexpectedly opened')
        except ssl.SSLCertVerificationError as error:
            result['failure']={'opened':False,'certificateRejected':True,'errorName':type(error).__name__,
                               'verifyCode':error.verify_code,'verifyMessage':error.verify_message}
        emit(result)
        return
    async with connect(args.uri,**options) as ws:
        require(ws.subprotocol=='chat' and ws.protocol.extensions==[],'negotiated protocol and no compression')
        require(ws.response.status_code==101 and ws.response.headers['x-interop']=='yes','actual Upgrade response')
        result['protocol']=ws.subprotocol
        if args.operation=='server-close':
            await ws.send('__close_server__')
            try:
                await ws.recv()
                raise AssertionError('unexpected data instead of server Close')
            except websockets.exceptions.ConnectionClosedError as error:
                require(error.rcvd is not None and error.sent is not None,'server Close handshake')
            await ws.wait_closed()
            result['close']=clean_close(ws,3001,'server\0终')
        else:
            for message in payloads(args.challenge):
                await ws.send(message)
                echoed=await asyncio.wait_for(ws.recv(),4)
                require(type(echoed) is type(message) and echoed==message,'independent client received exact echo')
                result['messages'].append(summary(echoed))
            if args.operation=='fragment-ping':
                for message,text in [('分片\0😀'.encode(),True),(bytes(range(251))*240,False)]:
                    async def fragments():
                        yield message[:1]
                        pong=await ws.ping(b'fragment-ping')
                        await asyncio.wait_for(pong,3)
                        result['pingPong']+=1
                        yield message[1:4]
                        yield message[4:]
                    await ws.send(fragments(),text=text)
                    echoed=await asyncio.wait_for(ws.recv(),4)
                    expected=message.decode() if text else message
                    require(type(echoed) is type(expected) and echoed==expected,'fragmented echo and interleaved Pong')
                    result['messages'].append(summary(echoed))
            await ws.close(1000,'client\0终')
            result['close']=clean_close(ws,1000,'client\0终')
            try:
                await ws.recv()
                raise AssertionError('extra message after expected echo sequence')
            except ConnectionClosedOK:
                pass
    emit(result)

async def peer(args):
    info=identity();records=[];active=set();sni={}
    def tls_context(cert,key):
        context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert,key)
        context.set_servername_callback(lambda obj,name,ctx:sni.__setitem__(id(obj),name))
        return context
    async def handler(ws):
        active.add(ws)
        url=urlsplit(ws.request.path);query=parse_qs(url.query)
        case=query['case'][0];challenge=query['challenge'][0]
        record={'case':case,'protocol':ws.subprotocol,'extensions':len(ws.protocol.extensions),
                'messages':[],'pingPong':0,'sni':sni.get(id(ws.transport.get_extra_info('ssl_object')))}
        records.append(record)
        try:
            require(ws.subprotocol=='chat' and ws.protocol.extensions==[],'outbound handshake protocol')
            for message in payloads(challenge):
                await ws.send(message)
                echoed=await asyncio.wait_for(ws.recv(),4)
                require(type(echoed) is type(message) and echoed==message,'Python peer received exact worker echo')
                record['messages'].append(summary(echoed))
            for message,text in [('分片\0😀'.encode(),True),(bytes(range(251))*240,False)]:
                async def fragments():
                    yield message[:1]
                    pong=await ws.ping(b'peer-fragment')
                    await asyncio.wait_for(pong,3)
                    record['pingPong']+=1
                    yield message[1:4]
                    yield message[4:]
                await ws.send(fragments(),text=text)
                echoed=await asyncio.wait_for(ws.recv(),4)
                expected=message.decode() if text else message
                require(type(echoed) is type(expected) and echoed==expected,'Python peer fragmented exchange')
                record['messages'].append(summary(echoed))
            await ws.close(1000,'remote\0终')
            try:
                await ws.recv()
                raise AssertionError('extra worker message after expected sequence')
            except ConnectionClosedOK:
                pass
            # close() waits for transport termination but doesn't raise on an
            # incomplete handshake. recv() above classifies an actual reset
            # before we assert the two Close frames of a successful session.
            record['close']=clean_close(ws,1000,'remote\0终')
        except Exception as error:
            record['failure']={'name':type(error).__name__,'message':str(error)}
            await ws.close(1011,'peer assertion failed')
        finally:
            active.discard(ws)
    options={'host':'127.0.0.1','port':0,'subprotocols':['chat'],'compression':None,
             'ping_interval':None,'close_timeout':2,'max_size':1024*1024}
    servers=[await serve(handler,**options),await serve(handler,ssl=tls_context(args.cert,args.key),**options),
             await serve(handler,ssl=tls_context(args.untrusted_cert,args.untrusted_key),**options)]
    emit({'type':'ready','identity':info,'ports':dict(zip(['ws','wss','untrusted'],[s.sockets[0].getsockname()[1] for s in servers]))})
    try:
        while True:
            line=await asyncio.to_thread(sys.stdin.readline)
            require(line!='','peer controller ended without stop')
            command=json.loads(line)
            if command['op']=='snapshot':emit({'type':'snapshot','active':len(active),'records':records})
            elif command['op']=='stop':break
            else:raise AssertionError('unknown peer command')
    finally:
        for server in servers:server.close()
        await asyncio.gather(*(server.wait_closed() for server in servers))
    emit({'type':'stopped','active':len(active),'records':records})
    require(not active and all('failure' not in r for r in records),'peer completes without active or failed sessions')

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('role',choices=['identity','client','peer'])
for name in ['uri','ca','case','challenge','operation','cert','key','untrusted-cert','untrusted-key']:
    parser.add_argument('--'+name)
args=parser.parse_args()
if args.role=='identity':emit(identity())
else:
    # In the pinned CPython 3.12.3 Windows Proactor transport, shutdown()
    # after a TCP reset may raise before close() and server._detach(),
    # leaving server.wait_closed() pending despite zero WebSocket sessions.
    # This peer uses TCP/TLS only (stdin is read via to_thread), so use the
    # bundled Selector loop on Windows. Keep the SDK/version/source lock,
    # all protocol assertions, callback errors and normal-exit checks intact.
    loop_factory=asyncio.SelectorEventLoop if sys.platform=='win32' else None
    asyncio.run(asyncio.wait_for(client(args) if args.role=='client' else peer(args),90),
                loop_factory=loop_factory)

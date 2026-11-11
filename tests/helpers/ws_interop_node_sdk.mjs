import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const lock=JSON.parse(fs.readFileSync(path.join(root,'tests/fixtures/websocket-interop-lock.json'),'utf8'));
const runtime=JSON.parse(fs.readFileSync(path.join(root,'tests/fixtures',lock.node),'utf8'));
export function loadNodeWS(packagePath=process.env.CAPSID_WS_INTEROP_NODE_PACKAGE) {
    assert.equal(process.version,runtime.client.node_version);assert.equal(process.versions.undici,runtime.client.undici_version);
    assert.ok(packagePath,'set CAPSID_WS_INTEROP_NODE_PACKAGE from tools/setup-ws-interop-python.py');
    const files=[];
    const walk=directory=>{
        for(const item of fs.readdirSync(directory,{withFileTypes:true})) {
            const absolute=path.join(directory,item.name);
            if(item.isDirectory())walk(absolute);
            else if(/\.(?:js|mjs|cjs)$/.test(item.name)||absolute===path.join(packagePath,'package.json'))
                files.push([path.relative(packagePath,absolute).split(path.sep).join('/'),createHash('sha256').update(fs.readFileSync(absolute)).digest('hex')]);
        }
    };
    walk(packagePath);files.sort(([a],[b])=>a<b?-1:a>b?1:0);
    const sourceSHA256=createHash('sha256').update(JSON.stringify(files)).digest('hex');
    const version=JSON.parse(fs.readFileSync(path.join(packagePath,'package.json'),'utf8')).version;
    assert.equal(version,lock.ws.version,'locked Node ws version');
    assert.equal(files.length,lock.ws.sourceFiles,'locked Node ws source inventory');
    assert.equal(sourceSHA256,lock.ws.sourceSHA256,'locked Node ws implementation');
    process.env.WS_NO_BUFFER_UTIL='1';process.env.WS_NO_UTF_8_VALIDATE='1';
    const WebSocket=createRequire(import.meta.url)(packagePath);
    return {WebSocket,WebSocketServer:WebSocket.WebSocketServer,
        identity:{node:process.version,undici:process.versions.undici,ws:version,sourceFiles:files.length,sourceSHA256}};
}

import {boundaryPeer} from './ws_fetch_transition_peer.mjs';
import {createHash} from 'node:crypto';
import {gzipSync} from 'node:zlib';

export async function byobPeer(kind,challenge,certificates){
    const match=/^chunks-(default|byob|least)-(\d+)-(\d+)$/.exec(kind);
    if(!match)throw new Error('Unknown reader probe');
    const body=Buffer.alloc(Number(match[2]));for(let index=0;index<body.length;index++)body[index]=(index*29+17)%251;
    const peer=await boundaryPeer('tls-body-reader-probe-eof',challenge,certificates,{bodyPrefix:body});
    return {...peer,snapshot(){
        const snapshot=peer.snapshot();
        for(const row of snapshot.records)row.sentChunks=row.sentChunks.map((value,index)=>{
            if(index===0||body.length<4096)return value;
            const raw=Buffer.from(value,'base64');return {encoding:'gzip-base64',rawBytes:raw.length,
                sha256:createHash('sha256').update(raw).digest('hex'),payload:gzipSync(raw,{level:9}).toString('base64')};
        });
        return snapshot;
    }};
}

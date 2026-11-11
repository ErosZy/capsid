import {boundaryPeer} from './ws_fetch_transition_peer.mjs';
import {createHash} from 'node:crypto';
import {gzipSync} from 'node:zlib';

// A leading byte is consumed by the measured BYOB reader itself. This proves
// that a held minimum test has reached the actual native body, before its
// bounded observation of a read which must remain pending.
export async function byobPeer(kind,challenge,certificates){
    if(kind.startsWith('body-')){
        const phase=kind.split('-').at(-1),length=phase==='empty'?0:phase==='small'?6:1048613;
        const bodyPrefix=Buffer.alloc(length);for(let i=0;i<length;i++)bodyPrefix[i]=(i*29+17)%251;
        const peer=await boundaryPeer('tls-body-reader-probe-eof',challenge,certificates,{bodyPrefix});
        // Preserve every actual body byte in a lossless transcript. Repeated
        // megabyte bodies should not multiply gate-control archive I/O.
        return {...peer,snapshot(){
            const snapshot=peer.snapshot();
            for(const row of snapshot.records)row.sentChunks=row.sentChunks.map((value,index)=>{
                if(index===0)return value;
                const raw=Buffer.from(value,'base64');return {encoding:'gzip-base64',rawBytes:raw.length,
                    sha256:createHash('sha256').update(raw).digest('hex'),payload:gzipSync(raw,{level:9}).toString('base64')};
            });
            return snapshot;
        }};
    }
    return boundaryPeer(kind.endsWith('-eof')?'tls-body-reader-probe-eof':'tls-body-reader-probe-object',
        challenge,certificates,{bodyPrefix:Buffer.from([0,102,105,114,115,116])});
}

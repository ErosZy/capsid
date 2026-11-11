import {boundaryPeer} from './ws_fetch_transition_peer.mjs';
export const nativeReaderLifecyclePeer=(kind,challenge,certificates)=>boundaryPeer(kind.endsWith('|release-eof')?'tls-body-reader-probe-eof':'tls-body-reader-probe-object',
    challenge,certificates,{bodyPrefix:Buffer.from([0,102,105,114,115,116])});

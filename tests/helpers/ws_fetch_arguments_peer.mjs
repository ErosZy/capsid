import {boundaryPeer} from './ws_fetch_transition_peer.mjs';

// Each argument row fetches its own real response. The entire transcript and
// independent nonce are retained for every row, including rejected reads.
export const byobPeer=(kind,challenge,certificates)=>boundaryPeer('tls-body-reader-probe-eof',challenge,certificates,
    {bodyPrefix:Buffer.from(Array.from({length:16},(_,index)=>index+1))});

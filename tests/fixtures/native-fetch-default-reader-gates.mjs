const plan = [7,4089,...Array(66).fill(4096),17];
const hash = view => {
    let value = 2166136261;
    for (const byte of view) value = Math.imul(value ^ byte, 16777619) >>> 0;
    return value.toString(16).padStart(8,'0');
};
const shape = value => value === undefined ? {type:'undefined'} : {
    type:value.constructor.name, offset:value.byteOffset, length:value.byteLength,
    backingLength:value.buffer.byteLength, valueHash:hash(value),
    backingHash:hash(new Uint8Array(value.buffer)), head:Array.from(value.slice(0,8)), tail:Array.from(value.slice(-8)),
};
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.endsWith('/health')) return new Response('healthy');
    if (!url.pathname.endsWith('/probe')) return new Response('missing',{status:404});
    const origin = url.searchParams.get('origin'), token = url.searchParams.get('token');
    const bodyKind = url.searchParams.get('body'), mode = url.searchParams.get('mode');
    const result = {token,bodyKind,mode,reads:[],phaseReadCounts:[],total:0,invalidBytes:0,closed:'pending',error:null};
    let response, reader;
    try {
        response = await fetch(`${origin}/data?token=${token}&body=${bodyKind}`,
            url.searchParams.get('transport') === 'upgrade' ? {headers:{Upgrade:'websocket'}} : undefined);
        result.status = response.status; result.bodyUsedBefore = response.bodyUsed;
        const ready = async () => {
            const ack = await fetch(`${origin}/ready?token=${token}`);
            if (ack.status !== 200 || await ack.text() !== 'ready') throw new Error('source header readiness failed');
        };
        if (mode === 'buffer-all-control') {
            // A real whole-body wait cannot acknowledge the held first part.
            const whole = response.arrayBuffer();
            await ready();
            result.wholeBodyBytes = (await whole).byteLength;
            result.unexpectedWholeBodyCompletion = true;
        } else {
            reader = response.body.getReader();
            reader.closed.then(()=>{result.closed='fulfilled';},()=>{result.closed='rejected';});
            const first = reader.read();
            await ready();
            for (let index=0;index<plan.length;index++) {
                let remaining = plan[index], reads = 0;
                while (remaining > 0) {
                    const read = index === 0 && reads === 0 ? await first : await reader.read();
                    const row = {phase:index,done:read.done,value:shape(read.value),closedAfterRead:result.closed};
                    result.reads.push(row);
                    if (read.done || read.value.byteLength === 0 || read.value.byteLength > remaining)
                        throw new Error(`unexpected read at part ${index}`);
                    for (const byte of read.value) {
                        if (byte !== (result.total*29+17)%251) result.invalidBytes++;
                        result.total++;
                    }
                    remaining -= read.value.byteLength;
                    reads++;
                }
                result.phaseReadCounts.push(reads);
                if (mode === 'cancel-after-first') {
                    await reader.cancel('gated-cancel');
                    await Promise.resolve();
                    result.closedAfterCancel = result.closed;
                    break;
                }
                // The origin cannot produce the next part before this fetch.
                const ack = await fetch(`${origin}/ack?token=${token}&part=${index}`);
                if (ack.status !== 200 || await ack.text() !== `ack:${index}`) throw new Error('acknowledgment failed');
            }
            if (mode === 'stream') {
                const read = await reader.read();
                result.eof = {done:read.done,value:shape(read.value)};
                await reader.closed;
                result.closedAfterEof = result.closed;
            }
        }
    } catch (error) {
        result.error = {name:error.name,message:error.message};
    } finally {
        if (reader) {
            try {reader.releaseLock();} catch (error) {result.releaseError={name:error.name,message:error.message};}
        }
        if (response) {result.bodyUsedAfter=response.bodyUsed;result.lockedAfterRelease=response.body.locked;}
    }
    return Response.json(result);
}};

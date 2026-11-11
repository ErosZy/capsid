export default () => {
    let resources = [], openSignal, settings;
    const snapshot = requestId => ({...settings, requestId: String(requestId), openSignalAborted: openSignal.aborted,
        resources: resources.map(item => ({slot: item.slot, status: item.response.status,
            bodyUsed: item.response.bodyUsed, locked: item.response.body.locked,
            headers: [...item.response.headers], hasReader: !!item.reader,
            closedPending: item.reader ? !item.closedSettled : null,
            firstPending: item.first ? !item.firstSettled : null, inputAfterIssue: item.inputAfterIssue}))});
    return {
        async open(input, call) {
            if (resources.length) throw new Error('fresh unread Binding pool required');
            settings = {stage: input.stage, count: input.count}; openSignal = call.signal;
            for (let slot = 0; slot < input.count; slot++) {
                const target = new URL(input.target); target.searchParams.set('slot', slot);
                const response = await fetch(target), item = {slot, response, inputAfterIssue: null,
                    closedSettled: false, firstSettled: false}; resources.push(item);
                if (input.stage !== 'unread') {
                    const byob = input.stage.includes('byob') || input.stage.includes('least');
                    item.reader = response.body.getReader(byob ? {mode: 'byob'} : undefined);
                    item.reader.closed.then(() => item.closedSettled = true, () => item.closedSettled = true);
                    if (input.stage.startsWith('pending-')) {
                        if (byob) {
                            const backing = new ArrayBuffer(24); new Uint8Array(backing).fill(165);
                            item.first = input.stage === 'pending-least' ?
                                item.reader.readAtLeast(2, new DataView(backing, 8, 8)) : item.reader.read(new Uint8Array(backing, 8, 8));
                            item.inputAfterIssue = backing.byteLength;
                        } else item.first = item.reader.read();
                        item.first.then(() => item.firstSettled = true, () => item.firstSettled = true);
                    }
                }
            }
            return snapshot(call.requestId);
        },
        state(_input, call) { return snapshot(call.requestId); },
    };
};

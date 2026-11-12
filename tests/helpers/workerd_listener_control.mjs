import {parseWorkerdListenPort} from '../../tools/workerd-fetch-byob-contract.mjs';

// The verified child binds port zero and reports its actual listener on fd 3.
// TCP readiness probes cannot establish which process owns a released port.
export function waitForWorkerdListen(child, milliseconds) {
    if (!Number.isFinite(milliseconds) || milliseconds <= 0) throw new RangeError('positive listener deadline');
    const control = child.stdio[3];
    return new Promise((resolve, reject) => {
        let buffer = '', finished = false;
        const finish = (error, value) => {
            if (finished) return;
            finished = true;
            clearTimeout(timer);
            control?.removeListener('data', data);
            control?.removeListener('error', failed);
            control?.removeListener('end', ended);
            child.removeListener('error', failed);
            child.removeListener('exit', exited);
            error ? reject(error) : resolve(value);
        };
        const failed = error => finish(error);
        const ended = () => finish(new Error('workerd listener control ended before readiness'));
        const exited = () => finish(new Error('workerd exited before listener readiness'));
        const data = chunk => {
            buffer += chunk;
            if (Buffer.byteLength(buffer) > 65536) return finish(new Error('oversized workerd listener control'));
            if (!buffer.includes('\n')) return;
            const line = buffer.slice(0, buffer.indexOf('\n'));
            try {finish(null, {port: parseWorkerdListenPort(line), line});}
            catch (error) {finish(error);}
        };
        const timer = setTimeout(() => finish(new Error('workerd listener control deadline')), milliseconds);
        if (!control || control.readableEnded || control.destroyed) return ended();
        if (child.exitCode !== null || child.signalCode !== null) return exited();
        control.setEncoding('utf8');
        control.on('data', data);
        control.once('error', failed);
        control.once('end', ended);
        child.once('error', failed);
        child.once('exit', exited);
    });
}

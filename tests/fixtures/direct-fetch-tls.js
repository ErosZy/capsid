async function expectTypeError(promise, label) {
    try {
        await promise;
    } catch (error) {
        if (error instanceof TypeError) {
            return;
        }
        throw new Error(`${label}: expected TypeError, got ${error?.name}`);
    }
    throw new Error(`${label}: fetch unexpectedly succeeded`);
}

export default {
    async fetch(request) {
        const url = new URL(request.url);
        const port = url.searchParams.get('port');
        const trusted = url.searchParams.get('trusted') === '1';
        const rsaPss = url.searchParams.get('rsaPss') === '1';

        try {
            if (rsaPss) {
                const response = await fetch(
                    `https://localhost:${port}/rsa-pss-tls12`);
                await response.text();
                if (response.status !== 200) {
                    throw new Error(
                        `RSA-PSS HTTPS response ${response.status}`);
                }
                return Response.json({ passed: true, mode: 'rsa-pss' });
            }

            if (!trusted) {
                await expectTypeError(
                    fetch(`https://localhost:${port}/without-custom-ca`),
                    'untrusted custom CA');
                return Response.json({ passed: true, mode: 'untrusted' });
            }

            const response = await fetch(
                `https://localhost:${port}/with-custom-ca`);
            const body = await response.text();
            if (response.status !== 200 ||
                body !== 'capsid tls ok') {
                throw new Error(
                    `trusted HTTPS response ${response.status}: ${body.slice(0, 80)}`);
            }

            // Connection reuse: the pooled TLS connection must serve the
            // second fetch without a new accept.
            const acceptsBefore = Number(
                await (await fetch(`https://localhost:${port}/accept-count`)).text());
            const pooled = await fetch(
                `https://localhost:${port}/with-custom-ca`);
            if (pooled.status !== 200 ||
                await pooled.text() !== 'capsid tls ok') {
                throw new Error(
                    `pooled HTTPS response ${pooled.status}`);
            }
            const acceptsAfter = Number(
                await (await fetch(`https://localhost:${port}/accept-count`)).text());
            if (acceptsAfter !== acceptsBefore) {
                throw new Error(
                    `HTTPS fetch must reuse the pooled TLS connection: ${acceptsBefore} -> ${acceptsAfter}`);
            }

            await expectTypeError(
                fetch(`https://127.0.0.1:${port}/hostname-mismatch`),
                'TLS hostname mismatch');

            return Response.json({ passed: true, mode: 'trusted' });
        } catch (error) {
            return Response.json({
                passed: false,
                name: error?.name,
                message: error?.message,
            }, { status: 500 });
        }
    },
};

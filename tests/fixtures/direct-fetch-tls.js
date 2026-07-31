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

        try {
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

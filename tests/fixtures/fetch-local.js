export default {
    async fetch(request) {
        const target = new URL(request.url).searchParams.get('target');
        try {
            const response = await fetch(target);
            const body = await response.text();
            return new Response(JSON.stringify({
                passed:
                    response.status === 200 &&
                    response.headers.get('x-capsid-upstream') === 'direct-egress' &&
                    body === 'capsid-fetch-ok',
                status: response.status,
                upstreamHeader: response.headers.get('x-capsid-upstream'),
                body,
            }), {
                headers: { 'content-type': 'application/json' },
            });
        } catch (error) {
            return new Response(JSON.stringify({
                passed: false,
                errorName: error?.name,
                errorMessage: error?.message,
            }), {
                headers: { 'content-type': 'application/json' },
            });
        }
    },
};

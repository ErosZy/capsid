// Exercise the embedding host's response flow control without an egress peer.
const alphabet = 'abcdefghijklmnopqrstuvwxyz0123456789';
export default {fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    const bytes = Number(url.searchParams.get('view'));
    if (![262143, 262144, 262145, 786469].includes(bytes)) throw new Error('unexpected response size');
    const text = alphabet.repeat(Math.ceil(bytes / alphabet.length)).slice(0, bytes);
    return new Response(text, {headers: {'content-type': 'text/plain'}});
}};

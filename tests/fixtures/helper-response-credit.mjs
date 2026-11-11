const alphabet = 'abcdefghijklmnopqrstuvwxyz0123456789';
export default {fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    const bytes = Number(new URL(url.searchParams.get('target')).searchParams.get('bytes'));
    if (![31, 32, 33, 101].includes(bytes)) throw new Error('unexpected credit probe size');
    return new Response(alphabet.repeat(Math.ceil(bytes / alphabet.length)).slice(0, bytes));
}};

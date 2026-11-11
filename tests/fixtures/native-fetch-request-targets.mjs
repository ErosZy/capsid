export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    const response = await fetch(url.searchParams.get('target'));
    if (url.searchParams.get('shape') === 'inline')
        return Response.json({status: response.status, body: await response.text()});
    const body = await response.text();
    return Response.json({status: response.status, body});
}};

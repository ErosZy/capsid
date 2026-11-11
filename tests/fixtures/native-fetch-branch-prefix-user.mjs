import echo from 'capsid:binding/echo';
export default {async fetch(request) {
    const url = new URL(request.url), [stage, count] = url.searchParams.get('views').split(',');
    const data = url.searchParams.get('phase') === 'open' ?
        await echo.open({target: url.searchParams.get('target'), stage, count: Number(count)}) : await echo.state({});
    return Response.json(data);
}};

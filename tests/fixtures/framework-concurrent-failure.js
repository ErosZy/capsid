export default {
    fetch(request) {
        const path = new URL(request.url).pathname;
        if (path === '/throw') {
            throw new Error('framework-concurrent-fault:/throw');
        }
        if (path === '/timeout') {
            return new Promise(() => {});
        }
        return new Response(`ok:${path}`);
    },
};

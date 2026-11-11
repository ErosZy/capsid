function recurse(depth) {
    return 1 + recurse(depth + 1);
}

export default {
    fetch(request) {
        if (new URL(request.url).pathname === '/overflow') {
            try {
                recurse(0);
                return new Response('recursion unexpectedly returned', {status: 500});
            } catch (error) {
                return Response.json({name: error.name});
            }
        }
        return new Response('worker-still-responsive');
    },
};

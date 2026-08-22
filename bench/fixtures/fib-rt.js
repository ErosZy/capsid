// Call-dense fixture: naive recursive Fibonacci. Every recursion level
// is an opaque call barrier, so the optimizer can only shrink the
// wrapper code — the honest floor of the optimization envelope.
export default {
    fetch() {
        function fib(n) {
            if (n < 2) return n;
            return fib(n - 1) + fib(n - 2);
        }
        let total = 0;
        for (let i = 0; i < 5; i++) {
            total = (total + fib(24)) % 1000000007;
        }
        return new Response(String(total));
    },
};

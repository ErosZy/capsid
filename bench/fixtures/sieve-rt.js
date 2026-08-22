// Typed-array / counter-loop fixture: sieve of Eratosthenes over a
// Uint8Array. Element accesses are opaque barriers; the honest
// mid-ceiling case.
export default {
    fetch() {
        const N = 300000;
        const sieve = new Uint8Array(N + 1);
        for (let i = 2; i * i <= N; i++) {
            if (sieve[i] === 0) {
                for (let j = i * i; j <= N; j += i) {
                    sieve[j] = 1;
                }
            }
        }
        let count = 0;
        let sum = 0;
        for (let i = 2; i <= N; i++) {
            if (sieve[i] === 0) {
                count++;
                sum = (sum + i) % 1000000007;
            }
        }
        return new Response(count + ':' + sum);
    },
};

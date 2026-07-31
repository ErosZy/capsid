export default {
    async fetch() {
        const tests = await globalThis.__wptCompletion;
        const expected = new Map(
            (globalThis.__wptExpectedFailures ?? []).map(item =>
                [ item.name, item.deviation ]),
        );
        const observedExpectedFailures = [];
        const failures = [];

        for (const test of tests) {
            if (test.status === 'PASS') {
                continue;
            }
            if (expected.has(test.name)) {
                observedExpectedFailures.push({
                    name: test.name,
                    deviation: expected.get(test.name),
                });
                expected.delete(test.name);
            } else {
                failures.push(test);
            }
        }
        const missingExpectedFailures = Array.from(
            expected,
            ([ name, deviation ]) => ({ name, deviation }),
        );
        /*
         * A realm that ran zero subtests must never report success. The upstream
         * source goes through concatenation, inline-script extraction, resource
         * mapping and an `arguments` rename before it reaches this realm; any of
         * those producing an empty program, or the harness being sealed before a
         * single test() call, would otherwise yield `passed: true` with an empty
         * failure list and silently remove this file from the conformance run.
         */
        const ranNothing = tests.length === 0;

        return Response.json({
            profile: 'CAPSID-MIN-2025-subset-v0',
            source: 'web-platform-tests/wpt',
            commit: '1985b47aa8972a970f005957f2bfa036da1787c6',
            passed:
                !ranNothing &&
                failures.length === 0 &&
                missingExpectedFailures.length === 0,
            ranNothing,
            total: tests.length,
            failures,
            expectedFailures: observedExpectedFailures,
            missingExpectedFailures,
        });
    },
};

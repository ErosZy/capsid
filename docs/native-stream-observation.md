# Native stream observations and request lifetime

An ordinary native fetch body can remain locked and have pending reads after its owning HTTP response ends. The upstream connection is retired with the request. The public read promises and stream locks retain the state observed in the pinned workerd profile.

A designated native stream future retains each subscriber's request identity without treating the parked observation as active continuation work. This includes internal default and BYOB read roots, native pipe waits, and async iterator reads that use the internal read algorithm directly. Ordinary application stream promises retain their existing accounting.

Native Promise forwarding and suspended async results propagate this designation only from an already designated pending native stream future. A late designation converts existing parked reactions under each original subscriber's context. Capture failure preserves the unconverted job lease, and conversion remains subject to the VM interrupt budget. Delivery still checks the subscriber's original scope. It does not grant authority to perform native I/O for another request.

Normal pending promises and timers remain subject to normal request lifetime checks. The regression controls retain the ordinary input, aggregate parent, and custom `then` handlers so that garbage collection cannot remove the work under examination. A read followed by a retained ordinary wait or timer still trips the existing terminal continuation guard. A completed ordinary wait or timer permits a clean exit.

The native tests use a real held raw or gzip body. They receive a three-byte decoded prefix and retire the actual owner response and upstream connection before submitting an observer request. The observer compares metadata, reader state, bytes, backing buffers, and locks with the pinned primary. Positive cases then complete a third healthy request in the same worker, account for every HTTP response head, terminal frame and returned byte credit, and observe a natural operating-system exit.

`native_retired_ownership_*` covers Promise adoption and combinators, tee and clone graphs, pipe and transform graphs, `values()` and `for await`, and ordinary-promise boundary controls. Each group has a 60-second CTest budget; the helper retains its five-second event budget and 2.5-second natural-exit budget. `native_retired_ownership_contract_controls` checks captured passing reports, actual old native failures, incomplete reports, coherent public-state corruption, fabricated source retirement, incorrect wire credits, and forced exits.

The QuickJS job-context unit keeps its original 20-second budget and original context tests. Additional properties cover late observation, async preludes, revoked subscribers, partial capture failure, ordinary mixed inputs, intrinsic forwarding, overridden `then`, and interruption during reaction conversion.

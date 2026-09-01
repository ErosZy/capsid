# Binding v1 Security Audit Remediation Plan

> Execution rule: every behavior change follows Red -> Green -> Refactor. A
> passing existing test is not evidence for a requirement unless the test
> exercises the real native entry point and proves both allow and deny paths.

**Goal:** Make the implementation satisfy `docs/binding-technical-design.md`
under an adversarial Binding package and adversarial User bundle, then replace
the document's current implementation claim with an evidence-backed acceptance
matrix.

**Architecture:** Keep one worker process with an optional User Runtime and one
Binding Runtime. The Binding Runtime is created only for a non-empty compiled
Binding set. User-to-Binding values cross only through the neutral asynchronous
RPC queues. Host policy is authoritative; every native operation is checked
against the active immutable Binding identity, and every native handle records
and enforces its owner identity.

**Non-negotiable boundary:** If a txiki.js module cannot be reduced to the v1
client-only surface and covered by real allow/deny tests, it is not grantable in
v1. Linux seccomp/Landlock are defense in depth and never substitute for the
runtime/native gate.

---

## Task 1: Pin and verify the complete READY sandbox proof

**Files:**

- Modify: `src/host/binding_compile.h`
- Modify: `src/host/binding_compile.cc`
- Modify: `src/host/managed_host.cc`
- Modify: `src/host/worker_executor.h`
- Modify: `src/host/worker_executor.cc`
- Modify: `src/worker_runtime.cc`
- Test: `tests/test_host_binding_compile.cc`
- Test: `tests/test_host_managed.cc`

**Red tests:**

- A Binding READY with correct profile digest but mismatched seccomp mode,
  Landlock ABI, or namespace identity is rejected.
- The managed warm path passes non-placeholder expected proof values.
- A replacement worker cannot be adopted after only the compatibility prefix
  matches.
- A zero-Binding worker keeps the exact baseline READY payload.

**Implementation:**

- Introduce one typed expected-proof value instead of three sentinel values.
- Obtain the launcher's actual seccomp mode, Landlock ABI, and namespace
  identity from the same launch result used to create the worker.
- Emit the actual namespace identity in Worker READY.
- Use the same full verifier in initial warm and replacement adoption.
- Never interpret zero/empty as a successful Binding proof.

**Verify:**

```sh
cmake --build build-audit --target test-host-binding-compile test-host-managed
ctest --test-dir build-audit -R 'host_binding_compile|host_managed_' --output-on-failure
```

## Task 2: Make Binding secret revisions part of generation identity

**Files:**

- Modify: `src/host/generation_identity.h`
- Modify: `src/host/generation_identity.cc`
- Modify: `src/host/binding_compile.h`
- Modify: `src/host/binding_compile.cc`
- Modify: `src/host/managed_host.cc`
- Test: `tests/test_host_binding_compile.cc`
- Test: `tests/test_host_secret_snapshot.cc`
- Test: `tests/test_host_managed.cc`

**Red tests:**

- Rotating only a Binding secret's opaque provider revision changes the
  Binding-set and generation digests.
- Changing secret bytes without changing the provider revision does not put
  secret bytes in any digest or committed snapshot.
- Recovery rejects committed Binding secret metadata that differs from the
  currently resolved provider revision.
- Multiple secrets and bindings are framed and sorted unambiguously.

**Implementation:**

- Compute a per-Binding opaque revision record from sorted
  `(public-name, key-id, provider-revision)` tuples with length framing.
- Persist only public names, key IDs, and opaque revisions in `bindings.json`.
- Recompute the record during recovery and compare it before warming workers.
- Add a Binding Runtime compatibility/version field to the Binding-set digest.

**Verify:**

```sh
cmake --build build-audit --target test-host-binding-compile test-host-secret-snapshot test-host-managed
ctest --test-dir build-audit -R 'host_binding_compile|host_secret_snapshot|host_managed_' --output-on-failure
```

## Task 3: Close grantable-module bypasses before exposing them

**Files:**

- Modify: `patches/txiki/0002-runtime-core.patch`
- Modify: `patches/txiki/0017-capsid-raw-egress-gate.patch`
- Modify: `patches/txiki/0018-capsid-fs-native-gate.patch`
- Modify/add: txiki overlay patches for WASI, SQLite, streams, TLS, UDP,
  readline, and Posix Socket
- Modify: `src/txiki_restricted_core.c`
- Modify: `src/worker_runtime.cc`
- Test: `tests/test_sandbox.cc`
- Test: `tests/test_txiki_vendor_patch_integrity.cmake`

**Red tests:**

- `capsid:wasi` cannot preopen an undeclared host path or attach undeclared stdio;
  its positive test instantiates and runs a real WASI module against an allowed
  preopen.
- `capsid:posix-socket` is either a usable, gated client-only facade or is absent
  from the v1 grantable set. `createFromFD`, `bind`, `listen`, `accept`, raw
  sockets, and AF_UNIX are always unavailable.
- Raw TCP/TLS/UDP, DNS, HTTP redirects, connection reuse, and reconnects each
  prove an allowed target succeeds and a denied target fails before syscall.
- `core.fs`, SQLite, readline/stdio, Pipe, and descriptor-taking APIs cannot
  exceed the current Binding's grants.
- User Runtime cannot import or indirectly obtain any of these native objects.

**Implementation:**

- Split the restricted internal core into an explicitly constructed Binding
  client namespace; do not expose server/listener/fd-adoption methods.
- Gate WASI options in native code. Because the current WAMR preopen API does
  not express read-only rights, require write permission for a preopen unless a
  verified read-only native implementation is added.
- Gate SQLite open paths and disable extension loading.
- Gate stdio at the native print/readline boundary.
- Remove any module from the manifest allowlist until its real operation tests
  pass.

**Verify:**

```sh
cmake --build build-audit --target test-sandbox txiki_vendor_patch_integrity
ctest --test-dir build-audit -R 'sandbox|txiki_.*(audit|integrity)' --output-on-failure
```

## Task 4: Enforce immutable native-handle ownership

**Files:**

- Modify: `include/capsid/capsid.h`
- Modify: `src/capability_policy.h`
- Modify: `src/capability_policy.cc`
- Modify: `src/txiki_restricted_core.c`
- Modify/add: txiki overlay handle-owner patch
- Modify: `src/worker_runtime.cc`
- Test: `tests/test_capability_policy.cc`
- Test: `tests/test_sandbox.cc`

**Red tests:**

- A File, Socket, TLS, UDP, SQLite, WASI, or Stream handle created by Binding A
  cannot be used by Binding B, by the User Runtime, or with no Binding token.
- Owner checks survive timers, promises, DNS callbacks, connection pools, and
  reconnect callbacks.
- Factory initialization has no valid Binding token and cannot create handles.

**Implementation:**

- Store a stable Binding owner token on every native wrapper at creation.
- Check owner equality and the operation policy on every native method, not
  only on open/connect.
- Make async callbacks retain/release a stable token object rather than copying
  a mutable global string.

**Verify:**

```sh
cmake --build build-audit --target test-capability-policy test-sandbox
ctest --test-dir build-audit -R 'capability_policy|sandbox' --output-on-failure
```

## Task 5: Finish RPC lifecycle, identity, and error semantics

**Files:**

- Modify: `src/worker_runtime.cc`
- Modify: `src/binding_rpc.h`
- Modify: `src/binding_rpc.cc`
- Test: `tests/test_binding_rpc.cc`
- Test: `tests/test_sandbox.cc`

**Red tests:**

- Calls are always queued; synchronous re-entry is impossible.
- Full 64-bit call IDs survive the JS callback round trip and never use ID 0.
- The call object contains the absolute request deadline.
- Sync throws use `JS_GetException`; Promise getter/call failures preserve a
  stable cloned error message.
- Undispatched cancellation releases the queue entry and quota; dispatched
  cancellation rejects once, aborts once, and is reclaimed even if the Binding
  promise never settles.
- Late settlement is dropped; per-request/per-worker quotas recover after every
  terminal path; poison clears all pending cross-runtime values.

**Implementation:**

- Give pending calls an explicit state machine and terminal reclamation path.
- Use BigUint64/BigInt consistently for call IDs.
- Duplicate/free every temporary QuickJS value exactly once.
- Add deadline-driven forced reclamation independent of Binding cooperation.

**Verify:**

```sh
cmake --build build-audit --target test-binding-rpc test-sandbox
ctest --test-dir build-audit -R 'binding_rpc|sandbox' --output-on-failure
```

## Task 6: Harden registry snapshots and Binding factory surface

**Files:**

- Modify: `src/host/binding_registry.cc`
- Modify: `src/host/binding_compile.cc`
- Modify: `src/worker_runtime.cc`
- Test: `tests/test_host_binding_registry.cc`
- Test: `tests/test_host_binding_compile.cc`
- Test: `tests/test_sandbox.cc`

**Red tests:**

- Root and package symlink, rename/replace, duplicate name, owner/mode, per-file
  size, and 64 MiB aggregate source limits fail closed.
- Recovery never re-reads `bindingsRoot` and rejects malformed/duplicate/extra
  snapshot fields.
- Config/secrets have null prototypes recursively and are deeply frozen.
- Factory method discovery never invokes getters/proxies and rejects accessors,
  symbols, non-functions, duplicate/confusable names, and excessive methods.
- Binding logs carry app, generation, Binding ID, request ID, and level without
  secret values.

**Verify:**

```sh
cmake --build build-audit --target test-host-binding-registry test-host-binding-compile test-sandbox
ctest --test-dir build-audit -R 'host_binding_(registry|compile)|sandbox' --output-on-failure
```

## Task 7: Linux enforcement gates and final evidence

**Files:**

- Modify: `tests/test_sandbox.cc`
- Modify: `.github/workflows/testing-validity.yml`
- Modify: `docs/binding-technical-design.md`

**Red tests:**

- Every declared profile has a real positive workload and a permanent-deny
  negative workload; negative probes require the expected denial (`EPERM` or
  native policy error), not merely any failure.
- Binding-enabled strict workers prove seccomp, Landlock, and namespace state;
  zero-Binding workers prove the unchanged baseline.
- CI fails if a mandatory privileged probe exits 77 or CTest reports skipped.

**Final verification:**

```sh
cmake -S . -B build-binding-audit -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-binding-audit -j4
ctest --test-dir build-binding-audit --output-on-failure
cmake --build build-asan-final -j4
ctest --test-dir build-asan-final --output-on-failure
cmake --build build-tsan-final -j4
ctest --test-dir build-tsan-final --output-on-failure
# Run the repository's privileged Linux runner; zero skipped Binding probes.
```

Update the design status only after the final requirement-by-requirement matrix
links every acceptance item to a real test and recorded command result. Do not
commit user-owned untracked benchmark files.

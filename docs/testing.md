# Testing and Continuous Gates

The project reports its own contracts, adapted WPT, process integration, and environment-based sandbox verification separately. Passing any one layer cannot substitute for another.

## Testing Layers

1. C/C++ unit tests: protocol, header, policy, topology, audit, and structured parsing;
2. real worker contract: bundle, IPC, flow control, cancellation, timeout, sandbox, and fetch;
3. pinned WPT: each upstream file selected by the manifest runs in an independent worker realm;
4. framework differential: Node reference and real worker are compared vector-by-vector, with independent absolute assertions retained;
5. sanitizer/fuzz: ASan and UBSan project targets, planned Host TSan, and four libFuzzer harnesses;
6. benchmark contract: verify content, versions, and environment before allowing performance samples to be recorded.

Standard sources, WPT selection, and deviations are unified in [Standards and Conformance](conformance.md).

## Validity Rules

Long-running gates are fail-closed:

- when WPT is not configured, register the `wpt_conformance_not_configured` failure sentinel;
- checkout must equal the pinned commit in the manifest;
- the manifest and the actually registered paths are compared item by item, not just by count;
- the WPT realm must report a top-level unique positive integer `total` and `ranNothing: false`;
- expected failures and `notExecuted` are read only from the manifest and must reference a registered deviation;
- restricted binary audit must first prove that the symbol table, archive, and LTO marker are readable;
- Wasm C, JavaScript, and fixture resource constants must be consistent;
- overlay key, stamp, manifest, and configure dependencies must come from the same canonical input;
- differential vectors from the three frameworks must have independent absolute assertions, and the anchor count cannot be zero;
- negative controls must prove that gates catch bad input, not only positive tests where a correct implementation turns green.

Test validity is continuously demonstrated by the automated gates above; one-off audit reports drift with current code, so they are not retained as product-state documents.

## Common Commands

Basic build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

Pinned WPT:

```sh
cmake -S . -B build-wpt \
  -DCMAKE_BUILD_TYPE=Release \
  -DCAPSID_ENABLE_LTO=ON \
  -DCAPSID_WPT_ROOT=/absolute/path/to/pinned/wpt
cmake --build build-wpt
ctest --test-dir build-wpt --output-on-failure
```

The `HEAD` of the WPT root must equal the commit in `tests/wpt/manifest.json`.

### Egress and TLS Targeted Regression

Domain egress rules authorize the addresses actually resolved for that domain, including private, loopback, link-local, and other protected ranges; explicit IP/CIDR deny still takes precedence. That authorization belongs only to requests initiated by domain name and does not turn the same address into a directly requestable IP literal. Policy unit tests and real worker regressions are covered by `egress_policy`, `worker_fetch_hostname_authorizes_resolved_loopback`, `worker_fetch_host_deny_diagnostic`, `worker_fetch_protected_deny_diagnostic`, `worker_fetch_explicit_deny_diagnostic`, and `worker_direct_fetch_http_matrix`. The final authorization semantics and the three diagnostic text types are described in [Host capability policy](capability-policy.md).

The pinned mbedTLS version once exhibited inconsistent signature-algorithm checks under a mixed TLS 1.2/1.3 client configuration: `mbedtls_ssl_get_pk_type_and_md_alg_from_sig_alg()` can parse the TLS signature scheme `0x0804` (`rsa_pss_rsae_sha256`), but the TLS 1.2 legacy hash/signature byte-pair check treats the high byte `0x08` as an unknown hash and rejects ServerKeyExchange. The client has already declared the scheme in ClientHello, and a TLS 1.2 server selecting it is legal behavior, so this is not a server or application configuration problem.

`patches/txiki/0009-lws-vendor.patch` only makes the TLS 1.2 client's ServerKeyExchange failure branch compatible with `rsa_pss_rsae_sha256/384/512`: each branch remains constrained by the corresponding hash and PKCS#1 v2.1 compilation capabilities, and the later "server-selected scheme must have actually been offered by the client" check is unchanged. The fix does not relax generic TLS 1.2 helpers, nor does it avoid negotiation by forcing TLS 1.2-only.

`worker_direct_fetch_https_tls12_rsa_pss` starts a local OpenSSL `s_server`, forces `-tls1_2 -sigalgs rsa_pss_rsae_sha256`, and then lets a real worker complete a trusted HTTPS request. On Linux it additionally registers `worker_strict_sandbox_https_tls12_rsa_pss`, which uses the same handshake regression to cover the `--strict` sandbox and requires the worker to exit cleanly; it is not registered on macOS, and like other strict-sandbox gates it is excluded from the ASan/TSan matrix. Targeted reproduction command:

```sh
ctest --test-dir build --output-on-failure \
  -R '^(egress_policy|worker_fetch_.*diagnostic|worker_fetch_hostname_authorizes_resolved_loopback|worker_direct_fetch_http_matrix|worker_direct_fetch_https_tls12_rsa_pss|worker_strict_sandbox_https_tls12_rsa_pss)$'
```

ASan example:

```sh
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCAPSID_ENABLE_ASAN=ON \
  -DCAPSID_USE_MIMALLOC=OFF
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure \
  -E '^(worker_strict_sandbox_direct_fetch|worker_strict_sandbox_https_ca|worker_strict_sandbox_https_tls12_rsa_pss)$'
```

UBSan replaces the switch with `CAPSID_ENABLE_UBSAN=ON`. Fuzz builds use Clang, `CAPSID_BUILD_WORKER=OFF`, and `CAPSID_BUILD_FUZZERS=ON`.

### TSan (M1C gate)

`CAPSID_ENABLE_TSAN` is configured: a dedicated Linux **GCC** Debug build that is not shared with ASan/UBSan (CMake configuration rejects the combination). It must pass before M1C acceptance; it is a mandatory gate before M2 multi-worker work begins.

TSan uses a dedicated Linux/GCC Debug build. Do not mix with ASan, UBSan, LTO, fuzz, or benchmark runs. **The supported TSan compiler in this environment is GCC**: Alpine/musl clang does not ship a TSan runtime (`libclang_rt.tsan_cxx.a` does not exist), so configuration rejects it and requires switching to GCC (`-DCMAKE_CXX_COMPILER=g++`). The first batch must cover at least command/event handoff between the HTTP event loop and worker threads, concurrent keep-alive, disconnect/cancel, timeout, and shutdown/reap. Any report from first-party code fails; third-party suppressions must be limited to specific external symbols, explain why, and cannot use broad rules to hide Host, Runtime, or IPC code. TSan results only prove race detection and are not QPS, latency, or CPU conclusions.

**Known coverage gap (compiler-diagnostic degradation, not race suppression)**: GCC 15.x's libstdc++ reports `-Werror=tsan` for `std::atomic_thread_fence` under `-fsanitize=thread` (Boost.Asio's fenced block uses that primitive). The build only downgrades that warning class to non-fatal (`-Wno-error=tsan`, inherited through the `capsid_sanitizers` INTERFACE); TSan instrumentation itself stays on—a probe with an injected race is still detected. The cost is that **TSan does not instrument `std::atomic_thread_fence` itself**, so races near fences may be missed; this is a documented diagnostic degradation and does not affect detection for all other synchronization primitives.

TSan has hard environment requirements: Clang TSan initialization must call `personality(ADDR_NO_RANDOMIZE)` to disable ASLR, so containers under the default Docker/containerd seccomp profile (including local bench/build containers) fail directly with a CHECK at `tsan_platform_linux.cpp:282`—this is not a code defect but an unmet environment prerequisite. The TSan gate must run on a real VM (GitHub hosted runner) or in a container with `--security-opt seccomp=unconfined`. In addition, TSan worker shadow memory mapping requires large contiguous virtual address space; sanitizer builds (`CAPSID_ASAN_BUILD` / `CAPSID_TSAN_BUILD`) disable the production RLIMIT_AS while keeping the QuickJS heap cap.

The CI `sanitizers` matrix adds a dedicated `tsan` entry: Clang compilation, `CAPSID_BUILD_HOST=ON`, and inside the job builds OpenSSL 3.5 from pinned source (SHA-256 verified) into `/opt/openssl35` (ubuntu-24.04 ships 3.0, which does not satisfy Host's 3.5 contract). The asan/ubsan entries keep their original configuration.

**The metrics-enabled path is the other half of the TSan gate**: M1C acceptance A/B evidence is generated with `CAPSID_HOST_IPC_METRICS=1`, and metrics are written by worker threads, read by the IO thread, and reset as a whole (`write_metrics_line` does `metrics_ = Metrics{}`); the lock-free cross-thread access is a real race (frozen RED: `host_single_worker_integration_metrics`). TSan passing with metrics off is not enough—the evidence path must also be race-free. `host_single_worker_integration_metrics` (ctest entry, `CAPSID_HOST_IPC_METRICS=1` environment) is an M1C gate alongside the ordinary integration tests.

## Platform Contract Gates

Platform gates separately prove "native development" and "production isolation"; they cannot replace each other:

- Linux Release is the v1 production gate and must verify strict sandbox, cgroup, and required READY feature bits in a delegated environment;
- macOS native-dev runs platform-neutral Host unit tests and real single-worker loopback integration; strict sandbox requests must fail as negative controls;
- Windows native-dev uses the `windows-latest` hosted runner: MSVC + Ninja builds Runtime, worker, bytecode compiler, and Host, runs the platform-neutral matrix, and actually starts Host and worker on the Windows host, covering source/trusted-bytecode identity, `capsid:env`, request, streaming, cancel, timeout, crash/reap, and loopback-only negative controls;
- Windows cross-compilation, Wine, WSL2, or a Linux container cannot substitute for hosted native Windows execution evidence;
- Windows/macOS native-dev passes must not be written as production sandbox passes; conversely, the Linux production gate cannot replace Windows development usability.

Windows supports single-worker and multi-shard static-pool: multi-shard scenarios run through a pool-level shared acceptor instead of `SO_REUSEPORT`. The managed Host remains unavailable, `capsid:fs` is degraded but usable, and strict sandbox is not implemented on Windows; see [Platform support overview](platform-support.md). Implementations must not turn green by skipping worker tests, disabling trusted bytecode, or replacing the listener with a non-native Linux VM.

## Environment-Based Sandbox Evidence

When a normal host lacks cgroup delegation or namespace permissions, the corresponding test returns CTest skip code 77. That only means the environment lacks the prerequisite; it cannot be recorded as a pass.

`.github/workflows/testing-validity.yml` contains five independent job classes:

- Ubuntu Release/LTO, pinned WPT, benchmark smoke, and privileged delegated sandbox, and generates the txiki.js upgrade report;
- Ubuntu ASan, UBSan, and TSan regular matrix; the TSan entry uses Clang and builds Host (including OpenSSL 3.5 built from local source); ASan excludes only two strict-sandbox network/TLS exit items already covered redundantly by the Release strict gate and the ASan non-strict same-function gate, because seccomp kills the process during instrumented runtime teardown;
- Clang/libFuzzer's four bounded corpus gates;
- macOS 14 POSIX host-library and non-worker unit matrix;
- `windows-latest` MSVC host-library, builds Runtime/worker/Host and runs platform-neutral tests; Linux-only tests are absent under the "not registered means skipped" or `SKIP_RETURN_CODE 77` principle.

All five job classes produce hosted evidence, uniformly gated by the final `hosted-evidence-index`; any job failure fails the whole run.

JUnit, build metadata, and upgrade reports are saved as workflow artifacts; generated report copies are not committed to the repository. The final `hosted-evidence-index` job downloads each job's evidence, writes the run URL, commit SHA, each evidence file's SHA-256, and an evidence tree summary, and requires all dependent jobs to succeed. The delegated sandbox script treats 77 as CI failure; ordinary runner environment skips cannot substitute for it.

All third-party actions are pinned to reviewed 40-character commit SHAs. The in-repo `testing_validity_workflow_audit` also locks the action list, four job classes, security gates, and CTest JUnit relative paths; `--test-dir` already determines the output root, and writing the build directory again into `--output-junit` is forbidden so reports and artifacts do not read a nonexistent two-level path.

## How Current Evidence Is Obtained

Test counts and results are based on the current build tree:

```sh
ctest --test-dir build --show-only=json-v1
ctest --test-dir build --output-on-failure
```

The pinned WPT file set is determined by `tests/wpt/manifest.json`; delegated sandbox positive tests must pass in a separate privileged environment and cannot be replaced by a normal host skip. CI generates a txiki.js upgrade report and hosted evidence index for each commit; those commit-bound artifacts with digests are the evidence for that run, and this document does not copy one day's counts.
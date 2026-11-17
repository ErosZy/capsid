# Documentation

Start with the task you want to complete. User guides explain how to use Capsid;
references define its contracts; dated reviews preserve what was observed for
specific inputs. A historical review is not a current deployment instruction.

The stable line is `0.2.2`; `0.3.0-beta.2` is an experimental prerelease.
[Current status and data scope](current-status.md) is the maintained entry point
for delivered scope, source identities and the latest accepted measurement.
[Release selection](releases.md) explains the channels. The
[qualification record](v0.3.0-current-qualification.md) binds beta acceptance to
its exact source and artifacts without claiming full standards conformance.

## Run an Application

| Task | Read |
| --- | --- |
| Install and serve a minimal Fetch handler | [Getting started](getting-started.md) |
| Select stable or beta; verify downloaded assets | [Releases and installation](releases.md) |
| Choose Linux, macOS or Windows | [Platform support](platform-support.md) |
| Request modules, egress and worker resources | [capsid.json guide](capsid-json.md) |
| Select a Host mode or configure managed applications | [Host configuration reference](host-config.md) |
| Use a Web-standard framework | [Framework compatibility](framework-compatibility/README.md) |

## Integrate and Secure a Host

| Task | Read |
| --- | --- |
| Understand the Host/worker split | [Architecture](architecture.md) |
| Drive the C/C++ runtime API | [Host integration](host-integration.md) |
| Define authority and resource rules | [Capability policy](capability-policy.md) · [Module permissions](module-permissions.md) |
| Enforce production isolation | [Linux strict sandbox](linux-sandbox.md) · [Security policy](../SECURITY.md) |
| Implement a trusted Binding package | [Binding modules](binding-modules.md) · [Binding design](binding-technical-design.md) |

Native development support is not a production isolation guarantee. The local
`single-worker` and `static-pool` examples use trusted application code;
`managed` and strict sandbox enforcement are Linux-only.

## Build and Verify

- [Source formatting](formatting.md): pinned C/C++, JavaScript/Python tools,
  shared write/check commands and preserved exact-byte inputs.
- [Building from source](building.md): toolchains, dependencies, clean Release
  inputs, development builds and SDK installation.
- [Windows](windows.md): native MSVC/static-CRT requirements and platform limits.
- [Testing guide](testing-guide.md): developer checks, full CI, raw results and
  prerequisite skips.
- [Standards and conformance](conformance.md): the selected Web API profile,
  reference revisions and explicit deviations.
- [Performance methodology](performance-benchmarks.md): benchmark protocol and
  source/build/environment identities, not a release-independent speed claim.
- [Contributing](../CONTRIBUTING.md): change scope, validation and documentation
  maintenance.

## Behavioral References

- [FetchRPC v4 wire contract](fetchrpc-v4.md): channel and Upgrade negotiation;
  do not infer an ABI or default protocol version from a document title.
- [workerd WebSocket semantics](workerd-websocket-semantics.md): observed
  behavior and the pinned compatibility boundary.
- [Native stream observation](native-stream-observation.md): body consumers and
  request lifetime.
- [Upgrade fallback gzip responses](native-upgrade-response-gzip.md).
- [Native iterator storage](native-iterator-storage.md).

## Release Evidence and History

Use [history and evidence](history/README.md) for dated qualification snapshots,
implementation plans, audits and agent handoffs. Original paths and archived
bytes are retained; old progress text does not supersede a later release record.
In particular, a passing beta campaign does not erase earlier failures or close
all historical v0.3.0 goals.

## How This Documentation Is Maintained

1. Put tutorials in guides, option/ABI semantics in references, and dated
   results in evidence records. Avoid appending execution diaries to tutorials.
2. Prefer public headers, capability manifests, build/configuration code and
   actual source-bound artifacts over prose when they conflict. A current
   checkout is not automatically the same input as a released binary.
3. Link every maintained Markdown document from this index, directly or through
   a linked sub-index. Keep existing anchors and paths unless migration is
   explicitly planned.
4. Preserve historical evidence, failed runs and their source identities. Never
   rewrite old counts, hashes, timing budgets or outcomes as current results.
5. Run `node tests/audit-current-docs.mjs .` after edits. It checks local links,
   index reachability, security guidance and SHA-256 of archived qualification
   snapshots. It is not a replacement for executing documented commands.

The [per-file coverage inventory](documentation-coverage.json) records the
content-review disposition and canonical-text/exact-evidence digest of every
page. Dated plans/reviews are explicitly marked historical. Six mixed journal
pages now have current reference fronts, with their original bytes and 483 old
anchors inventoried under [history](history/README.md). No old result is
silently relabeled as a current measurement.

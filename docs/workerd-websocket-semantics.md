# workerd WebSocket Semantics and Compatibility Scope

Reviewed 2026-11-17 against the published beta.2 source and locked reference.
This is a bounded compatibility reference, not a browser/workerd certification
or a pending-release diary. [Current status](current-status.md) identifies the
actual released source, raw standards conflicts and independent performance scope.

## Reference and Execution Boundary

- workerd release `v1.20261010.1`, source
  `8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f`;
- compatibility date `2026-08-20`, no explicit compatibility flags;
- platform-specific binaries, compressed assets and client/tool identities in
  the [reference lock](../tests/fixtures/workerd-ws/reference-lock.json) and
  [native Windows lock](../tests/fixtures/workerd-ws/reference-lock-windows.json);
- source-bound required gate in
  [`tools/run-workerd-ws-gate.mjs`](../tools/run-workerd-ws-gate.mjs).

The Linux comparison uses the reference and single-worker/static-pool/managed
Capsid backends. Native Windows/macOS qualification uses its actual supported
profile, not an assumption that managed mode exists there. Node ws and Python
websockets are independent clients/servers, not aliases for the same Node
implementation. TLS fixtures use the declared CA; verification is not disabled.

Required reports pin binaries, fixture/runner/client sources, exact case
inventories and natural exits. Empty/duplicate/filtered selection, missing
backends, zero assertions, malformed reports and unlisted differences fail the
gate. Portable metadata/control tests are not live reference evidence. Old
checkpoint test counts are retained in the historical snapshot, not displayed
as today's registration or acceptance count.

## Pair, Transfer and Acceptance

A pair exposes indexed endpoints and an iterator. The inbound idiom is:

```js
export default {
  fetch(request) {
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    return new Response(null, { status: 101, webSocket: client });
  },
};
```

This is a minimal bundle, not a complete Host launch; supply an actual Upgrade
request using the [quick-start tools/configuration](getting-started.md). Acceptance
and response coupling apply to different endpoints. Repeated ordinary accept
returns; accepting a coupled endpoint is an error. The option is
`allowHalfOpen`, not a hibernation-only extension.

A pair/Response transferred by an earlier HTTP request cannot be reused by a
later request. Fresh per-request pairs remain successful controls. A previously
accepted endpoint can reach a valid 101 and then abort the transport on both
engines; that is a commit failure, not successful endpoint coupling.

Response construction separates endpoint branding from transfer. Non-101 with
an endpoint throws RangeError. Fixed zero-length bodies become null; even an
already-empty ReadableStream is rejected for 101. Actual bytes determine body
emptiness, not a shadowed length/conversion property. Empty body adds no implicit
Content-Type; an explicit header is preserved. Upgraded Response cloning is
rejected.

101 can be visible before delayed local accept. Early DATA/ordinary Close stays
buffered until accept, and clean Close preserves preceding DATA. Transport
failure interrupts without delivering into an unaccepted endpoint. Capsid's
provisional coupling is bounded by the earlier of the request deadline and ten
seconds. Never-accept expiry and ACK/quota cleanup are explicit resource policy,
not an inferred reference parity claim.

## Events, Binary Values and Conversion

The event map is open/message/close/error, not messageerror. The pinned worker
profile defaults binaryType to `blob` under this date; a Node client's default
is a separate property. Invalid assignment retains the current value. Echo
fixtures select arraybuffer explicitly for byte comparisons.

send copies accepted buffer/view bytes before queueing. A Blob argument is not
a byte-preserving send primitive merely because it stringifies: the locked
behavior sends `[object Blob]`. Typed-array slice offsets, source mutation,
conversion count/order, Symbols and argument getter failures have independent
observations.

The pinned send string and close-reason conversions differ for unmatched UTF-16
surrogates: send can produce WTF-8 (`ed a0 80` for `\ud800`), while the USVString
close reason produces U+FFFD. A validating network client can disconnect at that
text frame. These raw observations do not make invalid UTF-8 standard-compliant.
Capsid's selected incoming malformed-text cases follow the reference's
replacement-character behavior; Close-reason UTF-8 has an independent 1007 gate.

Public EventTarget/listener/handler attributes follow the declared workerd
default profile. Throwing listeners, removal/re-addition, nested dispatch,
option reads, signal removal and ordinary/attribute listener identity have
separate controls. Arbitrary diagnostic text is not broadly normalized.

## Close and Lifetime

The locked legacy JS close-code set is 1000–4999 except 1004/1005/1006/1015;
flags/dates such as pedanticWpt can differ. Close reason has its declared byte
limit. Empty Close and transport failure are distinct; close while CONNECTING
cancels the pending operation.

Remote EOF without Close reports code 1006, wasClean=false and
`WebSocket disconnected without sending Close frame.`. The callback observes
OPEN; the next microtask/timer observes CLOSING. Operations on disposed
transports still perform required argument and origin checks. Clean Close,
establishment failure, protocol error and resource denial remain distinct.

Ordinary local Close ends the sending direction without discarding legal
opposite-direction assembly/delivery. A final allowed message from an
allowHalfOpen callback precedes reciprocal Close. CANCEL/transport terminal
barriers instead stop producers and suppress invalid late delivery. The Host
must drain actual accepted writes and stop the socket before ACK; an internal
phase notification is not an ACK/queue-publication proof.

An unfinished outbound constructor follows its HTTP owner through the response
body's end. Held success/rejection/stall controls retain the JS objects, prove
actual peer closure and require no later callbacks; GC is not a cancellation
substitute. Returning a Response object alone does not end a streamed body's
owner. Once its open event begins, an established socket can continue after the
HTTP body. A different request cannot reuse that native I/O origin, even when
the object remains reachable or closed.

Retained local incoming owners after transport EOF use the optional v4 lifetime
extension. Physical network reservations and local async liveness are separate,
bounded counts; managed generation drain deadlines still apply. Read the
[wire/ACK contract](fetchrpc-v4.md), [embedding guide](host-integration.md) and
[native stream lifetime](native-stream-observation.md).

## Outbound ws/wss

The global constructor uses Host-mediated DNS, address/egress checking,
TCP/TLS/SNI, Upgrade handshake and result barriers. Local fixture grants and
trusted CAs are explicit. Tests include invalid URL/protocols, pending cancel,
out-of-order results, coalesced handshake/data, large/fragmented messages,
invalid Accept, HTTP non-101/redirect, unknown CA and wrong hostname.

Before connection the URL retains its input and protocol/extensions are empty;
when none is selected the locked success observation uses null. Establishment
failure reports the case-specific Error/TypeError then terminal 1006 while
closed. Error objects and diagnostics are checked rather than being silently
replaced with a common string.

The reference accepts a server-selected protocol absent from the offered list;
Capsid rejects that under RFC 6455 section 4.1. This is an explicit product
security difference, not a missing comparison. Pending/result and channel/queue
budgets are finite. A constructor that cannot synchronously admit OPEN throws
rather than retaining an unannounced actor.

## Transport Scope and Limits

- Host-owned framing, masking, fragmentation and Ping/Pong are independent of
  JavaScript event semantics. Pings never become FetchRPC frames.
- Pong coalescing is legal. Ordered actual Ping payloads and interleaved data
  remain checked; a bounded observation ending its socket does not prove a
  watchdog killed a process.
- Capsid rejects invalid received Close codes with 1002 instead of reproducing
  the reference's selected invalid-code echo. Malformed Close-reason UTF-8 is
  checked separately.
- Default message and pending-send budgets are finite (the recorded default
  scope uses 1 MiB messages and a 4 MiB pending-send budget). A synchronous 8 MiB
  burst is resource-policy denial, not an unbounded retry queue. The reference
  default message limit is different.
- Compression, hibernation, raw listeners and cross-worker broadcast are not
  promised. Two-worker fanout probes prove room/instance isolation and native
  request ownership; they do not create a broadcast service.
- A precisely captured reference-only `Network connection lost.` ErrorEvent
  before a matching clean Close has its own exact allowance/count. Additional
  or altered errors, native errors or arbitrary ordering do not inherit it.

All stated differences remain visible with their source/date/flag scope. The
same code can be compatible with the selected reference and non-conformant to
a separate Web standard requirement.

## Standards, Qualification and Remaining Scope

The current [v2 workerd/WPT profile](../tests/wpt/workerd-default-profile-v2.json)
retains five raw standard-file failures, including exact nested-task timeout
conflicts. MessagePort's initially disabled standard queue, the workerd event
profile and Headers differences are documented in [conformance](conformance.md).
Ordinary suite success and full standards conformance remain false; the older
v1/two-file classification is not the current release policy.

The declared beta.2 full/native/sanitizer/fuzz/soak/package gates passed; see
[qualification](v0.3.0-current-qualification.md). This does not certify every
possible lifecycle/proxy graph, resolve missing historical schedules, grant
native macOS/Windows production isolation or turn a reference observation into
a universal specification. No new runtime/reference execution is claimed merely
because this document was refreshed. Current commands are in the
[testing guide](testing-guide.md).

## Historical source and preserved anchors

The original dated checkpoints and their numbers are preserved byte-for-byte in the
[captured document](history/archives/2026-11-17/workerd-websocket-semantics.md.txt). They are not current release instructions.

<details>
<summary>Legacy section links</summary>

<a id="workerd-websocket-semantics-dossier-v030-reference"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L1) — workerd-websocket-semantics-dossier-v030-reference

<a id="capsid-review-phase-107"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L8) — capsid-review-phase-107

<a id="第107轮2026-11-07sdk70与分支修复合入继续主线与干净构建验收"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L10) — 第107轮2026-11-07sdk70与分支修复合入继续主线与干净构建验收

<a id="capsid-review-phase-106"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L31) — capsid-review-phase-106

<a id="第106轮2026-11-07私有上下文与未读gzip分支验证进展"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L33) — 第106轮2026-11-07私有上下文与未读gzip分支验证进展

<a id="capsid-review-phase-105"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L47) — capsid-review-phase-105

<a id="第105轮更新2026-11-07"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L49) — 第105轮更新2026-11-07

<a id="capsid-review-phase-104-in-progress"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L59) — capsid-review-phase-104-in-progress

<a id="第104轮进行中2026-10-24主树480个定向组合补齐4处助手额度并定位运行时超时异常"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L61) — 第104轮进行中2026-10-24主树480个定向组合补齐4处助手额度并定位运行时超时异常

<a id="已合入主树4处c-api测试助手归还响应额度"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L67) — 已合入主树4处c-api测试助手归还响应额度

<a id="新发现快照正文在部分发送后超时取消会污染运行时异常状态"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L87) — 新发现快照正文在部分发送后超时取消会污染运行时异常状态

<a id="私有js-v7仍按原范围保留"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L97) — 私有js-v7仍按原范围保留

<a id="当前差异与继续顺序"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L103) — 当前差异与继续顺序

<a id="capsid-review-phase-103"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L112) — capsid-review-phase-103

<a id="第-103-轮2026-10-21byob-参数顺序标准容量拒绝与-abi-自检同步"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L114) — 第-103-轮2026-10-21byob-参数顺序标准容量拒绝与-abi-自检同步

<a id="capsid-review-phase-102"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L143) — capsid-review-phase-102

<a id="第-102-轮2026-10-21大体积读取defaultreader-分配及偏移-eof"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L145) — 第-102-轮2026-10-21大体积读取defaultreader-分配及偏移-eof

<a id="capsid-review-phase-101"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L176) — capsid-review-phase-101

<a id="第-101-轮2026-10-21读取器原型借用float16array-与-abi-测试归属修复"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L178) — 第-101-轮2026-10-21读取器原型借用float16array-与-abi-测试归属修复

<a id="capsid-review-phase-100"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L207) — capsid-review-phase-100

<a id="第-100-轮2026-10-21upgrade-回退-http-响应体的原生-byob-基础矩阵"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L209) — 第-100-轮2026-10-21upgrade-回退-http-响应体的原生-byob-基础矩阵

<a id="capsid-review-phase-99"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L238) — capsid-review-phase-99

<a id="第-99-轮2026-10-20原生-body-的直接-defaultreader-构造路径"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L240) — 第-99-轮2026-10-20原生-body-的直接-defaultreader-构造路径

<a id="capsid-review-phase-98"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L267) — capsid-review-phase-98

<a id="第-98-轮2026-10-20优化构建中的真实分配探针与完整-spawn-异常遍历"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L269) — 第-98-轮2026-10-20优化构建中的真实分配探针与完整-spawn-异常遍历

<a id="capsid-review-phase-97"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L292) — capsid-review-phase-97

<a id="第-97-轮2026-10-20自定义配置v4-初始化与真实资源回收"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L294) — 第-97-轮2026-10-20自定义配置v4-初始化与真实资源回收

<a id="capsid-review-phase-96"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L325) — capsid-review-phase-96

<a id="第-96-轮2026-10-20默认-abi-分配失败资源回收和测试覆盖修正"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L327) — 第-96-轮2026-10-20默认-abi-分配失败资源回收和测试覆盖修正

<a id="capsid-review-phase-95"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L339) — capsid-review-phase-95

<a id="capsid-review-phase-94"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L353) — capsid-review-phase-94

<a id="第-94-轮2026-10-20默认编码流栈修复完整三配置基线与新增差异"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L355) — 第-94-轮2026-10-20默认编码流栈修复完整三配置基线与新增差异

<a id="capsid-review-phase-93"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L374) — capsid-review-phase-93

<a id="第-93-轮2026-10-20五栈-qps-控制器与归档已接通"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L376) — 第-93-轮2026-10-20五栈-qps-控制器与归档已接通

<a id="capsid-review-phase-92"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L387) — capsid-review-phase-92

<a id="第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L389) — 第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续

<a id="capsid-review-phase-91"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L398) — capsid-review-phase-91

<a id="第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L399) — 第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续

<a id="capsid-review-phase-90"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L408) — capsid-review-phase-90

<a id="第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L409) — 第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续

<a id="capsid-review-phase-89"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L422) — capsid-review-phase-89

<a id="第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L423) — 第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成

<a id="capsid-review-phase-88"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L434) — capsid-review-phase-88

<a id="第-88-轮状态2026-10-20密度控制与档案生产器已接通"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L435) — 第-88-轮状态2026-10-20密度控制与档案生产器已接通

<a id="capsid-review-phase-87"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L446) — capsid-review-phase-87

<a id="第-87-轮状态2026-10-20修正性能采集与网页证据边界"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L447) — 第-87-轮状态2026-10-20修正性能采集与网页证据边界

<a id="capsid-review-phase-86"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L458) — capsid-review-phase-86

<a id="第-86-轮状态2026-10-20完成-windows-流栈压力的受控构建对照"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L459) — 第-86-轮状态2026-10-20完成-windows-流栈压力的受控构建对照

<a id="capsid-review-phase-85"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L470) — capsid-review-phase-85

<a id="第-85-轮状态2026-10-20新增-30-项转换与生命周期场景已完成专项闭环"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L471) — 第-85-轮状态2026-10-20新增-30-项转换与生命周期场景已完成专项闭环

<a id="capsid-review-phase-84"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L482) — capsid-review-phase-84

<a id="第-84-轮状态2026-10-20新增-fetch-边界已完成专项闭环"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L483) — 第-84-轮状态2026-10-20新增-fetch-边界已完成专项闭环

<a id="capsid-review-phase-83"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L494) — capsid-review-phase-83

<a id="第-83-轮fetched-response-原生耦合的已验证语义2026-10-20"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L495) — 第-83-轮fetched-response-原生耦合的已验证语义2026-10-20

<a id="capsid-review-phase-82"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L504) — capsid-review-phase-82

<a id="第-82-轮状态2026-10-20managed-更新保留本地连接来源修复-windows-增量缓存"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L505) — 第-82-轮状态2026-10-20managed-更新保留本地连接来源修复-windows-增量缓存

<a id="capsid-review-phase-81"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L516) — capsid-review-phase-81

<a id="第-81-轮状态2026-10-20延后本地关闭缺口已修复"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L517) — 第-81-轮状态2026-10-20延后本地关闭缺口已修复

<a id="capsid-review-phase-80"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L526) — capsid-review-phase-80

<a id="第-80-轮状态2026-10-20异常父连接的等待事件与本地-close"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L527) — 第-80-轮状态2026-10-20异常父连接的等待事件与本地-close

<a id="capsid-review-phase-79"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L534) — capsid-review-phase-79

<a id="第-79-轮状态2026-10-20共享-io-生命周期与完整必测门禁"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L535) — 第-79-轮状态2026-10-20共享-io-生命周期与完整必测门禁

<a id="capsid-review-phase-78"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L544) — capsid-review-phase-78

<a id="第-78-轮状态2026-10-20公开-upgrade-fetch-基本-13-场景已通过"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L545) — 第-78-轮状态2026-10-20公开-upgrade-fetch-基本-13-场景已通过

<a id="capsid-review-phase-77"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L552) — capsid-review-phase-77

<a id="第-77-轮当前进度2026-10-20upgrade-fetch-基础已验证公开链路仍待接通"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L553) — 第-77-轮当前进度2026-10-20upgrade-fetch-基础已验证公开链路仍待接通

<a id="capsid-review-phase-76"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L562) — capsid-review-phase-76

<a id="第-76-轮当前进度2026-10-19集合内建方法fetch-请求头与当前全量回归"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L563) — 第-76-轮当前进度2026-10-19集合内建方法fetch-请求头与当前全量回归

<a id="capsid-review-phase-75"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L576) — capsid-review-phase-75

<a id="第-75-轮当前进度2026-10-19webcrypto-digesthmac-与全量回归闭环"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L577) — 第-75-轮当前进度2026-10-19webcrypto-digesthmac-与全量回归闭环

<a id="workerd-websocket-semantics-dossier-v030-reference-1"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L588) — workerd-websocket-semantics-dossier-v030-reference-1

<a id="capsid-review-phase-74"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L591) — capsid-review-phase-74

<a id="第-74-轮当前进度2026-10-19超时-abort-桥恢复请求上下文"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L592) — 第-74-轮当前进度2026-10-19超时-abort-桥恢复请求上下文

<a id="capsid-review-phase-73"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L601) — capsid-review-phase-73

<a id="第-73-轮当前进度2026-10-19fetch-调用者上下文恢复"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L602) — 第-73-轮当前进度2026-10-19fetch-调用者上下文恢复

<a id="capsid-review-phase-72"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L611) — capsid-review-phase-72

<a id="第-72-轮当前进度2026-10-19fetch-资源回收与取消语义"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L612) — 第-72-轮当前进度2026-10-19fetch-资源回收与取消语义

<a id="capsid-review-phase-71"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L621) — capsid-review-phase-71

<a id="第-71-轮当前进度2026-10-19跨上下文-promise"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L622) — 第-71-轮当前进度2026-10-19跨上下文-promise

<a id="capsid-review-phase-70"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L631) — capsid-review-phase-70

<a id="第-70-轮当前进度2026-10-19异步回调与执行预算"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L632) — 第-70-轮当前进度2026-10-19异步回调与执行预算

<a id="capsid-review-phase-69"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L641) — capsid-review-phase-69

<a id="第-69-轮当前进度2026-10-19连接定时器与-promise-生命周期"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L642) — 第-69-轮当前进度2026-10-19连接定时器与-promise-生命周期

<a id="capsid-review-phase-68"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L675) — capsid-review-phase-68

<a id="第-68-轮当前进度2026-10-19原生异常重建与内建字符串转换"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L676) — 第-68-轮当前进度2026-10-19原生异常重建与内建字符串转换

<a id="capsid-review-phase-67"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L709) — capsid-review-phase-67

<a id="第-67-轮当前进度2026-10-19普通异常对象克隆与引擎类型判断"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L710) — 第-67-轮当前进度2026-10-19普通异常对象克隆与引擎类型判断

<a id="capsid-review-phase-66"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L743) — capsid-review-phase-66

<a id="第-66-轮当前进度2026-10-19异常-getter-与错误监听器的-io"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L744) — 第-66-轮当前进度2026-10-19异常-getter-与错误监听器的-io

<a id="capsid-review-phase-65"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L776) — capsid-review-phase-65

<a id="current-checkpoint-handler15b-synchronous-exception-snapshots"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L777) — current-checkpoint-handler15b-synchronous-exception-snapshots

<a id="capsid-review-phase-64"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L790) — capsid-review-phase-64

<a id="current-checkpoint-handler14i-accepted-writes-and-trusted-errors"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L791) — current-checkpoint-handler14i-accepted-writes-and-trusted-errors

<a id="capsid-review-phase-63"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L804) — capsid-review-phase-63

<a id="current-checkpoint-accepted-frames-before-a-synchronous-throw"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L805) — current-checkpoint-accepted-frames-before-a-synchronous-throw

<a id="pair-endpoint-transfer-and-accept"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L860) — pair-endpoint-transfer-and-accept

<a id="events-binary-values-and-send"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L879) — events-binary-values-and-send

<a id="close-and-pending-connection-cancellation"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L919) — close-and-pending-connection-cancellation

<a id="transport-observations-and-remaining-acceptance"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L959) — transport-observations-and-remaining-acceptance

<a id="implemented-inbound-scope-and-explicit-deviations-2026-10-16"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L994) — implemented-inbound-scope-and-explicit-deviations-2026-10-16

<a id="outbound-constructor-observations-2026-10-13-continuation"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L1037) — outbound-constructor-observations-2026-10-13-continuation

<a id="additional-compatibility-observations-2026-10-15"></a>
[Historical section](history/archives/2026-11-17/workerd-websocket-semantics.md.txt#L1093) — additional-compatibility-observations-2026-10-15

</details>

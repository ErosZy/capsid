# FetchRPC v4 contract

> Reviewed against the v0.3.0-beta.2 codec, registry and public header on
> 2026-11-17. ABI v7 and the default v3 entry remain unchanged; v4 is explicitly
> negotiated and never mixed with v3 on one connection.

This is the current byte/state reference. Early design/checkpoint journals have
been separated from it, not reinterpreted as present-day blockers. The declared
beta.2 gates passed; references below to bounded phase observations describe
their evidence scope, not an unpublished release. Broader graphs and historical
causal gaps remain limited to their own evidence.

Source authority: [protocol definitions](../src/protocol.h),
[channel registry](../src/channel_registry.h), [public ABI](../include/capsid/runtime.h)
and [embedding guide](host-integration.md). Release and measured-source identities
are in [current status](current-status.md). Golden payload bounds and original
assertions are not changed by this documentation refresh.

## 1. Framing and negotiation

### 1.1 Frame layout (unchanged from v3)

```text
offset  size  field
0       4     magic bytes 57 52 54 43         (0x43545257, LE)
4       2     version (u16)                  v4 frames: 4
6       2     type   (u16)
8       4     flags  (u32)
12      8     request_id (u64)
20      4     payload_size (u32)             <= 64 KiB
24      …     payload
```

### 1.2 Hello v4 (amended after the payload audit)

The host's `kHello` carries the worker startup configuration in v3; v4
appends a capability tail to that payload instead of replacing it (the
draft's fixed-87-byte shape ignored the config fields — amended
2026-10-12, recorded here):

```text
[ v3 Hello payload: startup config, variable length, >= 108-byte fixed prefix ]
-8      4     capabilities (u32 LE bitset, tail)
-4      4     reserved (u32, must be 0)
```

The worker answers its capabilities on `kReady`: in v4 the READY
payload appends `u32 worker_capabilities, u32 reserved(0)` after the
existing compatibility-id + sandbox-proof bytes. **Effective
capabilities are the conjunction** of both sides' bitsets; a side that
did not announce a feature must not emit its frames. Validation reads
the tail (last 8 bytes).

Capability bits (bit index → meaning):

| bit | name | effect |
| --- | --- | --- |
| 0 | `CAP_DUPLEX_CHANNEL` | channel frames (§3) are legal |
| 1 | `CAP_UPGRADE` | Upgrade accept/reject ops (§2) are legal |
| 2 | `CAP_HALF_CLOSE` | `kChannelHalfClose` is legal |
| 3 | `CAP_CHANNEL_LAYOUT_A2` | the A2 payloads (§3.6) are in force |
| 4 | `CAP_CHANNEL_CONFIRM` | v4.2 result/terminal ACK and pending-dial CANCEL (§3.5.1) |
| 5 | `CAP_CHANNEL_CLOSE_FINAL` | atomic final Close and transport barrier; requires CONFIRM (§3.5.1) |
| 6 | `CAP_FETCH_UPGRADE` | optional HTTP upgrade metadata/body channel (§3.2a); implemented in the qualified first-party path; broader proxy/context graphs separately scoped |
| 7 | `CAP_CONNECTION_LIFETIME` | optional incoming local-owner retention after transport DISCONNECT (§3.2b); requires UPGRADE and CONFIRM |
| 8..31 | reserved | **must be zero**; a nonzero reserved bit or an unknown capability bit is a fatal protocol error |

**Dependency closure (v4.1):** `CAP_DUPLEX_CHANNEL`, `CAP_UPGRADE`,
and `CAP_HALF_CLOSE` each require `CAP_CHANNEL_LAYOUT_A2`.
`CAP_UPGRADE`, `CAP_HALF_CLOSE` and `CAP_CHANNEL_CONFIRM` additionally require
`CAP_DUPLEX_CHANNEL` and A2. Announcing `CAP_CHANNEL_LAYOUT_A2` without
`CAP_DUPLEX_CHANNEL` is a capability error. A bitset that violates the
closure is rejected as `invalid hello capabilities` — on both the
Hello and the READY tail.
`CAP_CHANNEL_CLOSE_FINAL` additionally requires `CAP_CHANNEL_CONFIRM`,
and therefore DUPLEX and A2; this closure is checked on both tails.
`CAP_FETCH_UPGRADE` additionally requires UPGRADE, CONFIRM and CLOSE_FINAL,
and therefore DUPLEX and A2 (complete minimal mask 123).
`CAP_CONNECTION_LIFETIME` additionally requires UPGRADE and CONFIRM,
and therefore DUPLEX and A2 (complete minimal mask 155). It does not require
FETCH_UPGRADE or CLOSE_FINAL. The known mask is 255; the first-party effective
mask is 251 because HALF_CLOSE is not advertised.

Allocation rule: a feature is usable only when both sides announce it.
Both announcements must satisfy the dependency closure, so their intersection
also satisfies it. A frame for a non-negotiated feature fails with
`channel frame without capability` even when its type is legal.

#### 1.2.1 Hello A2 limits extension (v4.1)

When (and only when) the Hello's capability tail carries
`CAP_CHANNEL_LAYOUT_A2`, a length-delimited extension sits **between**
the v3 startup payload and the capability tail:

```text
[ v3 startup payload, variable ]
u32     extension_size = 48
u16     extension_version = 1
u16     reserved = 0
u32     max_channels             1 ..= 1024
u64     max_message_bytes        1024 ..= 64 MiB (67108864)
u64     reassembly_budget_bytes  1 MiB ..= 256 MiB
u64     send_budget_bytes        1 MiB ..= 256 MiB
u32     close_timeout_ms         100 ..= 600000
u32     dial_timeout_ms          100 ..= 600000
u32     idle_timeout_ms          0 (= disabled) ..= 86400000
u32     per_channel_queue_bytes  64 KiB ..= 4 MiB
u32     capabilities             (tail follows immediately)
u32     reserved = 0
```

All fields LE. Validation, in order: `extension_size` must be exactly
48 and leave exactly 8 bytes (the tail) after it;
`extension_version` must be 1; `reserved` must be 0; every bound above
must hold; no trailing bytes may remain. Any failure is
`invalid hello extension`. A Hello that announces A2 without the
extension, or carries the extension without A2, is likewise
`invalid hello extension`. A valid Hello under A2 must also pass the
real v3 startup decoder on its prefix — an all-zeros placeholder is
not a valid startup payload.

### 1.3 Version negotiation matrix (deterministic rejection)

A parser rejects, as its **first** action on the offending frame, with
`kParseError` and the exact error string below; no partial state may
survive:

| wire | v3 parser (shipped) | v4 parser |
| --- | --- | --- |
| version 3 frame | parse per v3 | `unsupported protocol version` |
| version 4 frame | `unsupported protocol version` (frozen by `test_rejects_unknown_type_and_version`) | parse per v4 |
| version <3 or >4 | `unsupported protocol version` | same |
| v4 Hello with reserved ≠ 0 or unknown capability bits | (version rejected first) | `invalid hello capabilities` |

Peers never downgrade and never mix versions on one connection.
"Upgrading" a connection means a new connection.

## 2. Upgrade (requires `CAP_UPGRADE`)

An inbound HTTP request carrying `Upgrade: websocket` (after host
validation, including `Sec-WebSocket-Key`/`-Version` per RFC 6455) is
forwarded to the worker as a normal v3-shaped `RequestHead` **plus**
the frame flag `kFlagRequestUpgrade` and an A2 upgrade tail appended
to its payload — `u32 channel_id` (pre-assigned by the host at
admission; id authority stays with the host; no worker-minted ids, no
open round trip) and `u64 max_message_bytes`. The worker answers
exactly one of:

- `kResponseHead` with status 101 **and** `kFlagResponseUpgrade`,
  whose payload appends the A2 response tail — `u32 channel_id` (the
  same id) and `u64 accepted_max_message_bytes`; the effective channel
  message cap is `min(request tail, response tail)`. The HTTP response
  headers the host writes are the RFC 6455 accept handshake
  (`Sec-WebSocket-Accept`), produced by the host; the selected
  subprotocol is extracted from the worker's response headers by the
  host, which confirms it is a member of the offered list and exactly
  one.
- a normal non-101 response — reject; the host answers the upgrade
  with that response, the pending id reservation is released, and no
  channel exists. `kResponseEnd` follows as for any normal response.

The tail follows the **complete** legacy head. RequestHead has
`u16 method_size, method, u32 url_size, url, u16 header_count, headers`.
An upgrade ResponseHead has `u16 status, u16 status_text_size, status_text,
u16 header_count, headers`, then the 12-byte tail. Each header is
`u16 name_size, name, u32 value_size, value`. Status is not a u32;
status-text and header-count prefixes must not be omitted. Both heads use
the same nonzero HTTP request id. The response tail must commit the pending
channel belonging to that request. After 101, no ordinary ResponseEnd is sent;
HTTP accounting ends and channel traffic uses header request_id = 0.

The C ABI `upgrade_begin` reserves off-wire. The next `begin_request` or
`begin_bodyless_request` for that HTTP id sends the actual head and tail.
Queue refusal preserves the reservation and all credit for retry. Cancelling
an unannounced reservation performs local cleanup without emitting a frame
for an id the worker has never seen.

The successful 101 ResponseHead is the commit point from pending to
open. A non-101 response to an upgrade-flagged request releases the
reservation; no channel frame for that id may follow (violation
`unknown channel`). `kFlagResponseUpgrade` is mutually exclusive with
`kFlagResponseFixedBody` on the same frame. Any other response to an
upgrade-flagged request (status 101 without `kFlagResponseUpgrade`,
the upgrade flag on a non-ResponseHead, upgrade tails on responses to
non-upgrade requests) is a protocol violation
(`invalid upgrade response`).

## 3. Duplex channels (requires `CAP_DUPLEX_CHANNEL`)

### 3.1 Identity and lifecycle

- `channel_id` is a **u32 assigned by the host** (the worker never
  mints ids) from a monotonic counter starting at 1 within one IPC
  connection, in announcement order; 0 is never a channel, ids are not reused, and the
  `request_id` namespace of v3 is untouched. When the u32 space
  exhausts, new channels are refused and the supervisor replaces the
  worker in an orderly shutdown.
- The worker requests outbound channels with `kChannelOpen`
  (`channel_id = 0`, nonzero `dial_id`, target URL, and candidate
  subprotocols). Dial ids are an independent, monotonic u64 sequence;
  they cannot be reused within the IPC connection, including after rejection
  or cancellation. The host assigns the id when announcing the result and answers `kChannelAccept`
  (with the selected subprotocol) or `kChannelReject` (with a reason
  code and a bounded diagnostic). Inbound upgrades never send OPEN:
  the RequestHead upgrade tail (§2) is the open.
- Ownership (exactly one per channel, fixed at open): `handshake`
  (pending upgrade, pre-101), `duplex-connection` (post-101 inbound),
  `request`, `response-stream`, `standalone` (outbound dial), `tail`
  (reserved; creating one is refused in v0.3.0). The owner kind is
  carried in OPEN and drives cleanup (§3.4).

### 3.2 Frame table (types are stable; payloads below are A2, v4.1)

| type | name | direction | payload (A2) |
| --- | --- | --- | --- |
| 19 | `kChannelOpen` | worker→host only | `u32 channel_id(=0), u64 dial_id(!=0), u8 owner_kind, u8 channel_kind, u16 reserved(=0), u64 owner_request_id, u64 max_message_bytes, u32 url_size, url bytes, u16 protocol_count, [u16 size + bytes]*` |
| 20 | `kChannelAccept` | host→worker only | `u64 dial_id, u32 channel_id(!=0), u64 accepted_max_message_bytes, u16 selected_size, bytes` |
| 21 | `kChannelReject` | host→worker only | `u64 dial_id, u32 channel_id, u32 reason_code, u16 diagnostic_size, bytes` |
| 22 | `kChannelData` | both | `u32 channel_id`, then message fragment bytes (≤ 65532 per frame; flags = start/end/text) |
| 23 | `kChannelCredit` | both | `u32 channel_id, u64 credit` |
| 24 | `kChannelHalfClose` | both (requires `CAP_HALF_CLOSE`) | `u32 channel_id` |
| 25 | `kChannelClose` | both | `u32 channel_id, u8 close_kind, u8 flags, u16 code, u16 reason_size, reason bytes (≤ 123)` |
| 26 | `kChannelCancel` | both | `u32 channel_id(!=0), u32 terminal_reason` |
| 27 | `kChannelAck` | both (requires CONFIRM) | result/terminal acknowledgement (§3.5.1) |
| 28 | `kConnectionLifetime` | worker→host only (requires CONNECTION_LIFETIME) | `u32 channel_id(!=0), u32 retained(0 or 1)` (§3.2b) |

String prefixes: URL `u32`, subprotocol and diagnostic `u16`. Every
channel frame carries `request_id = 0` in the header; ids come from
the payload only. Grammar-level bounds (violation
`invalid channel payload`): truncation, trailing bytes, inconsistent
lengths, `reserved != 0`, `owner_kind`/`channel_kind` outside the
frozen enums, `protocol_count > 64`, any subprotocol or the
selected/diagnostic string `> 256` bytes, `max_message_bytes` outside
`[1024, 64 MiB]`, Data payload `< 4` bytes, and CANCEL
`terminal_reason > 5`.

Frozen enums:

```text
owner_kind: 0 handshake, 1 duplex-connection, 2 request,
            3 response-stream, 4 standalone, 5 tail (refused), 6..255 invalid
channel_kind: 0 websocket; 1 fetch-upgrade only with CAP_FETCH_UPGRADE;
              without that capability 1..255 invalid; 2..255 always invalid in OPEN
dial reason (REJECT): 0 unspecified, 1 egress-denied, 2 dns-failure,
            3 connect-refused, 4 tls-failure, 5 handshake-rejected,
            6 protocol-violation, 7 dial-timeout, 8 resource-limits,
            9 cancelled
cancel terminal_reason: 0 local, 1 disconnect, 2 timeout,
            3 shutdown, 4 resource-limits, 5 protocol-violation
```

OPEN direction rules (violation `channel open direction`): OPEN
arriving at the host must have `channel_id = 0` and `dial_id != 0`;
OPEN arriving at the worker, ACCEPT/REJECT arriving at the host, OPEN
with nonzero `channel_id` or zero `dial_id` are all rejected. A REJECT
for a dial that has not been assigned an id keeps `channel_id = 0`.

Types 15–18 stay allocated to `kAudit`, `kMemoryMetricsRequest`,
`kMemoryMetricsResponse`, and `kLoadBinding` (v3); v4 does not
renumber them. The Upgrade flags ride the existing head frames:
`kFlagRequestUpgrade` = bit 5 (value 32) on `kRequestHead`,
`kFlagResponseUpgrade` = bit 5 (value 32) on `kResponseHead`. Flag
namespaces are per frame type, as in v3. Data flags: `kFlagStart` (1),
`kFlagEnd` (2), `kFlagChanText` (4) on `kChannelData` only.

### 3.2a Optional FETCH_UPGRADE metadata (2026-10-20)

This extension is implemented in the codec, registry, additive C ABI and
first-party JS/Worker/Host bridge. First-party peers now negotiate bit 6.
Phase 78 verifies thirteen basic public fetch cases. Phase 81 verifies
nineteen shared-I/O-origin lifetime cases, including deferred incoming
local outgoing close after transport loss, with or without a sibling.
Complete native required gates and bounded retained-owner controls are
verified. Broader context graphs, managed generation retention, direct
proxy and transport qualification remain open; these bounded results do
not qualify the whole release.

With negotiated `CAP_FETCH_UPGRADE`, OPEN kind 1 appends to the unchanged
A2 prefix and empty subprotocol list:

```text
u16 metadata_version (=1), u8 redirect (0 follow / 1 manual / 2 error),
u8 reserved (=0), http_headers
```

The matching ACCEPT keeps an empty selected_protocol, then appends:

```text
u16 metadata_version (=1), u16 status (101 or 200..599),
u8 redirected (0/1), u8 reserved (=0),
u16 status_text_size, status_text bytes (<=1024),
u16 final_url_size, final_url bytes (nonempty, <=2048), http_headers

http_headers = u16 count (<=128),
               [u16 name_size, name bytes, u16 value_size, value bytes]*
```

The entire header encoding, including its count and length prefixes, is
bounded to 48 KiB. Names are nonempty ASCII HTTP tokens; values are counted
ByteString octets (no NUL/CR/LF or other field controls except HTAB).
Order and duplicates are retained, including separate Set-Cookie fields.
The complete payload still fits the 64 KiB frame bound. Truncation,
trailing bytes, bad versions, reserved fields or bounds are
`invalid channel payload`.

Metadata is required exactly for the matching kind-1 OPEN and forbidden
on ordinary constructor dials. Old codec callers default to no new
capability and preserve exact old bytes. Status 101 resolves to ordinary
WebSocket transport while retaining its fetch authority. Other statuses
resolve to registry-only kind 2 (HTTP body); kind 2 is never legal in OPEN.
Body DATA is binary Host-to-Worker only; credit is Worker-to-Host only.
204/205/304 admit no DATA. Successful body EOF is Host CLOSE_FINAL without
WebSocket code/reason and cannot truncate an open DATA message; its
transport ACK and cancellation barriers remain mandatory.

`capsid_worker_dial_accept_http()` takes a separately sized/versioned
descriptor, synchronously copies counted inputs, and transactionally
preserves pending state on queue refusal. Existing ABI-v7 layouts and the
ordinary dial_accept operation remain unchanged. The old typed DIAL view
rejects a new kind rather than silently discarding HTTP fields. The first-party
Host now performs capability-aware typed propagation through the versioned
HTTP descriptor. Basic public fetch/101/non-101/stream/abort/redirect behavior
is verified in phase 78. Phase 81 verifies nineteen shared-I/O-origin
lifetime cases and the complete native required gates. Incoming transport
EOF retains its locally open outgoing owner; local close drains queued
jobs before cancelling the final origin's native resources. Retention has
finite admission and explicit shutdown controls. Broader HTTP/context/
resource graphs and direct response proxy coupling still require
implementation or qualification.

### 3.2b Optional CONNECTION_LIFETIME (2026-10-20)

With negotiated bit 7, type 28 has exactly eight little-endian payload bytes,
zero flags and zero header request id. State 1 retains an already committed
incoming duplex owner after Host DISCONNECT and before its terminal ACK.
Pending upgrades, outbound dials, other terminal reasons, duplicate retains,
unknown ids and malformed states are rejected without changing the set.
The retained set is separate from physical channels and bounded by
`2 * max_channels`. Network ACK still releases physical capacity.

State 0 releases an existing retained id, including after its wire channel
was erased. Unknown or duplicate releases are rejected. Worker emits retain
before cancellation ACK when the incoming local outgoing direction remains
open; local close and the queued-job checkpoint finish the owner and emit
release. Worker teardown clears the set. This feature adds no network data,
credit or WebSocket close frame and does not change their existing formats.

ABI v7 appends `CAPSID_EVENT_CONNECTION_LIFETIME = 19` without changing any
existing structure layout. Its payload is the validated eight-byte control,
and `credit` is the range-checked total retained-owner count. The Host consumes
it as generation liveness and publishes physical plus retained ownership as
one atomic value; existing physical-channel metrics keep their meanings.
The original generation drain deadline still overrides retention.

Old embedders omitting bit 7 receive no type-28 frame or event 19. Legacy
Hello masks 0/31/63 preserve READY masks 0/27/59; new masks
123/155/251/255 produce 123/155/251/251. Matching first-party builds request
251. Older workers can reject this new capability; no automatic downgrade
or compatibility with an old worker advertising unknown bits is implied.

### 3.3 Credits and buffering

- Each direction maintains an independent u64 byte credit over
  **message bytes** (Data payload minus the 4-byte id prefix). Data may
  be sent only when credit ≥ fragment bytes; sending without credit is
  a protocol violation (`channel credit underflow`).
- Initial credit (both directions): 64 KiB (65536). Credit is
  replenished by `kChannelCredit` (additive; a replenish that would
  overflow u64 is `channel credit overflow`). Replenish arrives when
  the receiver has accepted a fragment into reserved reassembly space
  or completed the network write — not merely on receipt.
- With initial 65536 plus an additional 65536 grant, sending 65537
  message bytes is legal when no more than 65535 of that total credit was
  already consumed: credit arithmetic is over the running
  balance, not the initial window.
- Per-channel receive buffering is capped at 4 MiB (v3
  `kMaxBufferedBytes`); a sender pushing a peer over the cap is
  throttled by credit exhaustion, never by silent drop.
- Effective message cap per channel: `min(OPEN.max_message_bytes,
  ACCEPT.accepted_max_message_bytes)` (dial) or `min(request tail,
  response tail)` (upgrade), further bounded by the Hello A2
  `max_message_bytes`. A complete message larger than the cap is a
  protocol violation (`message size exceeded`). Frames carry at most
  65532 message bytes; larger messages MUST be fragmented.

### 3.4 Ownership cleanup matrix

| event | request-bound | response-stream-bound | tail-bound | standalone |
| --- | --- | --- | --- | --- |
| client disconnect | host sends `kChannelClose(1006-substitute code 1001)` then frees | same | survives until tail ends | host sends Close, frees |
| request timeout | host sends `kChannelClose(1001)`, frees | same | per tail deadline | unaffected |
| worker shutdown (clean) | worker sends Close for every open channel before `kShutdown` | same | same | same |
| worker replacement | host closes channels with code 1001, does not deliver to the old worker | same | same | same |
| peer Close received | deliver Close event, channel freed after both directions quiesce | same | same | same |

Half-close (`kChannelHalfClose`) ends one direction; the channel frees
when both directions are closed or half-closed in baseline A2. With CONFIRM,
HalfClose stops DATA only: CREDIT may still follow it, and the entry remains
until both full Close markers or a terminal ACK. For WebSockets,
half-close maps to "stop sending" (the JS API does not expose it in
v0.3.0; it exists on the wire for transport correctness).

### 3.5 Sequencing rules

- `kChannelData`/`Credit`/`HalfClose`/`Close`/`Cancel` referencing an
  unknown or already-freed id: protocol violation
  (`unknown channel`). Internal tombstones may absorb provable late
  completions of already-finished async work; they never excuse a wire
  frame that references a dead id.
- State machine per channel (worker-side dials mirrored):
  `dialing → (open | rejected)` for outbound dials;
  `pending → open` on the 101 ResponseHead for upgrades (a non-101
  response releases the pending reservation);
  `open → closing → closed` on Close/CANCEL. In `open`, each direction
  is independently `open` or `half-closed`.
- Frame order per channel is strict: (reservation/accept) → Data* →
  (HalfClose) → Close. Violations (`channel sequence`): Data on a
  pending (uncommitted) upgrade id or provisional v4.2 ACCEPT; a second Open for a live dial;
  Data after the **sender's** Close or HalfClose; a second
  same-direction HalfClose; a second same-direction Close.
- Fragmentation: a first Data fragment without `kFlagStart`, a
  `kFlagStart` while a message is open (nested start), a non-final
  fragment after `kFlagEnd`, a message whose fragments disagree on
  `kFlagChanText` (message type), or an end-flagless final fragment
  sequence that never closes: violation (`fragmentation`).
  `start → continuation → end` is the legal multi-fragment shape; a
  single frame with start|end set is a legal single-fragment message.
- Simultaneous close is legal: each side sends its own Close; the
  first Close received moves the channel to closing, and the peer's
  Close completes it. Duplicate Close from the **same** side is
  `channel sequence`.
- CREDIT is additive by definition (duplicates are simply two grants);
  no other frame is idempotent on the wire.

### 3.5.1 Confirmation amendment (v4.2, 2026-10-13)

`CAP_CHANNEL_CONFIRM` (bit 4) requires DUPLEX + A2 and is effective only if
both endpoints advertise it. Without it, ACK and pending-dial CANCEL fail
`channel frame without capability`. Production WebSocket adapters must require
this bit; baseline A2 alone lacks safe cancellation of an unassigned dial.
When the host offers CONFIRM, the production worker advertises DUPLEX, UPGRADE, A2 and CONFIRM (27), adding CLOSE_FINAL, FETCH_UPGRADE and CONNECTION_LIFETIME only when offered by the host and valid under their dependency closures (first-party mask 251). Without CONFIRM it advertises zero feature bits. HALF_CLOSE is not advertised.

`kChannelAck = 27`, zero flags and zero header request id, has exactly:

```text
u32 channel_id
u64 dial_id
u32 kind: 0 dial-result, 1 cancel, 2 transport-close
```

| kind | identity | direction and meaning |
| --- | --- | --- |
| 0 | nonzero dial id; assigned channel id for ACCEPT, zero for REJECT | worker→host: result consumed; ACCEPT becomes Open |
| 1 | assigned channel id / dial id zero, OR channel id zero / pending dial id | opposite side to the CANCEL sender; its producer has stopped |
| 2 | assigned channel id / dial id zero | opposite side to a transport Close; its producer has stopped |

Unknown kinds, ambiguous/empty identities, trailing bytes and malformed lengths
are `invalid channel payload`. Wrong result/pending-cancel ACK direction is
`channel open direction`. An unsolicited, duplicate or mismatched ACK is
`channel sequence` while the correlation exists, `unknown channel` after it is
reclaimed. Confirmations are never silently idempotent.

**Result barrier.** OPEN keeps a bounded dial record. ACCEPT assigns a fresh
host id but stays `AwaitingAck`; neither direction emits DATA/CREDIT/HalfClose
or assigned CANCEL until worker ACK kind 0 is queued. Host must receive that
ACK before producing DATA. REJECT retains only a result correlation until the
same ACK (channel id zero). Host cannot start an assigned terminal operation on
a provisional ACCEPT; it retains a transport failure and terminates after the
result barrier, or aborts IPC when the confirmation deadline expires.

**Pending cancellation.** Only the worker may cancel a locally unresolved
OPEN. Its CANCEL is the versioned 16-byte form:

```text
u32 channel_id = 0
u32 terminal_reason                 // unchanged enum 0..5
u64 dial_id != 0
```

The worker stops its dial producer and retains the correlation. Host receiving
it cancels resolver/connect/TLS/handshake IO and queues ACK kind 1 with channel
id zero and that dial id. After observing CANCEL, host must emit no further
ACCEPT/REJECT. A result already queued on the host stream may cross CANCEL;
the cancelled worker validates it, suppresses JS delivery and does **not**
send result ACK. An accepted result creates only provisional drain state,
never a revived JS connection. No DATA can precede result ACK, so provisional
connections have no unbounded network-to-worker queue. Host cancellation ACK
is ordered after every previously queued result. Both sides erase provisional
channel/result state at that marker. A processed result must use its ordinary
result ACK and, if accepted, subsequent assigned-channel cancellation instead.

**Assigned cancellation / transport failure.** The existing assigned CANCEL
remains exactly 8 bytes. CANCEL or transport Close ends its sender's producer,
releases owned queues and enters Cancelling. The opposite stream may contain
legal DATA/CREDIT/Close already queued before it learned about the terminal
marker. Validate that stream's fragmentation, credit and cap; drain its bytes
without payload allocation or JS callbacks. The receiver stops its producer
and queues the corresponding ACK after its earlier writes. The initiator
retires state when it consumes that ACK. Simultaneous terminal operations need
both ACKs; sending an ACK alone cannot erase one's own outstanding terminal
request. No new DATA/CREDIT/HalfClose/Close may be produced after observing a
CANCEL; the ACK defines the end of the peer's stream.

**Ordinary Close / HalfClose.** Close ends the sender's DATA and CREDIT stream,
not the opposite stream. A first Close must preserve opposite-direction
fragment validation state until that sender ends or confirms its stream.
Queued DATA crossing Close is valid and remains deliverable until the peer's
Close ends that receive direction; DATA
following the same sender's Close is invalid. After sending full Close, the
only further channel frame permitted is an ACK for a crossed peer terminal
operation. In CONFIRM mode both half-closed directions still keep the control
stream; mixed Close/HalfClose is not terminal until the second full Close or
confirmed CANCEL/transport failure.

**Atomic final Close (2026-10-18).** `CAP_CHANNEL_CLOSE_FINAL` (bit 5)
requires CONFIRM and is used only when both peers negotiate it. Close kind
`3` carries the same sendable code/reason (or absent code) as ordinary Close,
but also enters Cancelling and requires a transport-close ACK (kind 2).
The network adapter writes previously accepted messages and this Close in
order, stops the actual socket without waiting for a peer Close reply, then
acknowledges termination. Write failure and existing write/close deadlines
also tear down the socket before that ACK. Synthetic codes 1006/1015 remain
invalid for kind 3 and must never be written to the WebSocket wire.

This is one first Close marker, not a second Close following an ordinary
Close. Duplicate Close and DATA after the sender's Close remain invalid.
An ordinary peer Close crossing this final marker cannot reclaim the channel
until its terminal barrier is confirmed. Baseline A2 and CONFIRM peers that
did not negotiate bit 5 reject kind 3 with `channel frame without capability`.
The payload layout and existing kinds 0/1/2 retain their previous meaning.

2026-10-13 lifecycle correction (v4.2.1, no byte-layout change): local ordinary
Close must preserve owned opposite-direction assembly and message delivery.
The pinned workerd `lifecycle.mjs` probe verifies reception after local Close
and the final message from an `allowHalfOpen` close callback (7 assertions).
Suppression applies to CANCEL/transport terminal barriers, not ordinary Close.

**Bounds and deadlines.** The pending/result map is bounded by max-channels, separately from the
admitted-channel bound of the same size; rejected results reserve no live connection.
No per-completed-id tombstone is retained. OPEN uses the dial timeout; a result,
CANCEL, transport Close or incomplete full Close uses the close timeout. The
endpoint advances a monotonic clock. Missing confirmations expire the entire
IPC stream, release queues/correlations and forbid subsequent application:
`channel confirmation timeout`. This explicit failure prevents dropping a
record and later treating a legitimate queued ACK as an unrelated frame.
Adapters must drive the clock and prioritize bounded terminal writes. A
WOULD_BLOCK response spends neither credit nor confirmation state.

`tests/test_channel_races.cc` has separate host/worker registries and two FIFO
queues, independently scheduled reads: CANCEL before result, ACCEPT/REJECT
crossing CANCEL, result-before-DATA, queued fragmented DATA crossing Close,
assigned CANCEL and transport Close, simultaneous CANCEL, Close/CANCEL,
HalfClose/CREDIT, duplicate/unknown/mismatched identities and deadline cleanup.
The raw-peer C ABI test repeats pending/crossed cancellation over real IPC,
checks retry after a full writer and verifies a later dial on the same stream.

### 3.6 Close kinds and codes (v4.1)

`kChannelClose` payload freezes:

```text
u8 close_kind: 0 initiated (this side started the Close handshake)
               1 observed  (a peer Close frame was received)
               2 transport (the transport died without a Close)
               3 final     (write Close then end transport; requires bit 5)
u8 flags:     bit 0 (1) code-present; bits 1..7 must be zero
u16 code:     meaningful only when code-present, else must be 0
u16 reason_size + bytes (≤ 123, UTF-8 per the dossier)
```

Code matrix (violation `invalid close code`):

| close_kind | code when present |
| --- | --- |
| 0 initiated | 1000–4999 except 1004, 1005, 1006, 1015 (the sendable set) |
| 1 observed  | 1000–4999 except 1004, 1005, 1006, 1015 (what a lawful peer may put on the wire) |
| 2 transport | 1006 or 1015 only (synthesized observations) |
| 3 final | the sendable set, as for initiated Close; requires negotiated bit 5 |

Codes 1005/1006/1015 are receive-side observations and must never be
sent as network Close frames; on the IPC plane 1006 appears only with
`close_kind = 2`. A code-present flag with `code = 0`, or any
code outside the matrix, is `invalid close code`.

The network adapters use sendable Close payloads for WebSocket Close frames;
kind 2 is an IPC transport observation and emits no network Close frame.
Kind 3 also ends the actual transport after accepted frames drain and
requires the terminal ACK described above. Code and reason limits follow
the workerd dossier (`docs/workerd-websocket-semantics.md` §4).

### 3.7 Error table (v4.1, exact strings)

| string | scope | trigger |
| --- | --- | --- |
| `unsupported protocol version` | connection | version mismatch (§1.3) |
| `invalid hello capabilities` | connection | unknown/reserved bits, closure violation (Hello or READY tail) |
| `invalid hello extension` | connection | A2 limits extension malformed or out of bounds |
| `invalid channel payload` | frame | payload grammar: truncation, trailing bytes, lengths, enums, reserved, bounds |
| `channel open direction` | frame | OPEN/ACCEPT/REJECT direction or id misuse |
| `channel credit underflow` | channel | Data without sufficient credit |
| `channel credit overflow` | channel | replenish exceeding u64 |
| `message size exceeded` | channel | complete message over the negotiated cap |
| `unknown channel` | channel | id not live (never existed, freed, rejected, cancelled) |
| `channel sequence` | channel | order violations (§3.5) |
| `fragmentation` | channel | message assembly violations (§3.5) |
| `invalid close code` | frame | close kind/code matrix violation |
| `invalid upgrade response` | request | upgrade answer shape violations (§2) |
| `channel frame without capability` | frame | frame type legal but feature not negotiated |
| `channel confirmation timeout` | connection | a bounded dial/result/terminal confirmation did not arrive |

Channel-scoped errors tear down the offending channel; frame-scoped
grammar errors and connection-scoped negotiation errors tear down the
connection. Errors surface to embedders through REJECT, CANCEL, or the
existing kError channel with explicit scope — never silently.

## 4. Ping/pong

Owned entirely by the host transport; no FetchRPC frame exists. The
worker never sees pings (dossier §5).

## 5. Test vectors

Golden encodings and sequences live in
`tests/fixtures/fetchrpc-v4/vectors.json` (`schema_version: 2`,
schema documented in the file). The harness
`tests/test_fetchrpc_v4.cc` parses it with a real JSON parser
(vendored jansson), rejects missing files, wrong schemas, and
unexecuted sequences, asserts the shipped v3 parser rejects every
v4-versioned vector (the never-mix-silently boundary), round-trips
every vector through the v4 parser, and executes every sequence
against an independent reference state model (test-local; the
production registry repeats the same sequences in T06 and does not
substitute for it). Sequences cover: negotiation accept/reject matrix
(including the closure rules and A2 extension negatives), upgrade
accept and reject (with reservation release), fragmented messages,
credit exhaustion/replenish/overflow and the 65536+65536 window,
half-close, simultaneous close, unknown channel, duplicate close,
out-of-order dial completion, multi-channel interleave, message-cap
violations, cancel, and capability gating.

## 6. Settled design decisions (plan D1–D6)

| # | decision | outcome |
| - | --- | --- |
| D1 | WebSocket framing lives in the **host** (minimal RFC 6455 codec, property-tested). The worker never frames; txiki's `ws.c` stays unexposed (it would bypass egress mediation). |
| D2 | `channel_id` is **host-assigned**, u32, monotonic from 1; the worker only requests ids (0). |
| D3 | Half-close is a **dedicated frame** (`kChannelHalfClose`); the v0.3.0 JS API does not expose it. |
| D4 | Ping/pong is **transport-internal** on both planes; no FetchRPC frame, no JS surface. |
| D5 | ABI stays **v7, additive** (`capsid_worker_upgrade_*`, `CAPSID_EVENT_CHANNEL_*` on the event pump). Escalate to v8 only with a recorded embedder-blocking reason. |
| D6 | `WebSocketPair`/`WebSocket` ship in **`CAPSID-MIN`** with the workerd method surface; v0.5 adds `env`/`ctx` around it, not a different WebSocket. |

## 7. ABI surface (informational)

The implemented additive v7 entries include `capsid_worker_spawn_ex`,
`capsid_worker_upgrade_begin`, dial accept/reject and HTTP response descriptors,
channel send/grant/close/cancel/confirm, explicit/counted Close and protocol-info
queries. Existing config/event layouts remain frozen. See the current
[embedding reference](host-integration.md) and [public header](../include/capsid/runtime.h),
not an older draft's proposed `upgrade_respond` symbol. The ordinary spawn entry
remains v3; v4 capability negotiation is explicit.

## Executable-contract notes

- Corrected the magic annotation and encoded complete legacy head prefixes in
  the vectors, preserving frame types and v3 bytes.
- Clarified monotonic id announcement, independent u64 dial ids, request/response
  ownership matching, and terminal reservation cleanup.
- Enforced zero channel-frame header request ids, zero A2 OPEN flags, bounded
  REJECT reason codes, and no reason on an empty initiated/observed Close.
- Repaired JSON lifetime and reset each sequence target before replay. All
  steps, including steps after a rejected frame, compare actual outcomes.
  The JSON schema remains v2 because its object shape is unchanged; the schema
  description records the corrected fixture revision.
- The v4.2 confirmation amendment adds a negotiated result barrier and terminal
  barrier, with two-peer Close/cancel transcripts and bounded deadline cleanup.
  The worker IO core now validates startup/READY/steady-state and releases owned
  queues on shutdown; the first-party native JS connection roots and network adapters are implemented.
  Completed beta acceptance and genuinely open historical scopes are separated in
  the [qualification record](v0.3.0-current-qualification.md).

Inbound upgrade cancellation uses the assigned-channel CANCEL barrier after
the RequestHead announcement. A queued opposite-direction 101 for that
handshake is validated and suppressed, then the terminal ACK removes the
correlation. An HTTP error/non-101 answer releases an unresolved handshake;
a committed duplex channel survives ordinary HTTP token cleanup.

<!-- capsid-review-phase-64 -->
<a id="capsid-review-phase-64"></a>
### 2026-10-18 transport drain and peer compatibility

The Host drains complete accepted socket writes before acknowledging a worker transport Close, without adding a network Close frame. Existing write/close deadlines still bound failure cleanup. The final-Close kind retains the Close frame and does not wait for a peer reply.

This optional capability does not change old unknown-bit rejection policy. First-party Host/Worker are delivered as matching builds; old clients omitting bit 5 negotiate their previous feature set and receive no kind 3. An old worker may reject a new Host's HELLO, and no automatic wire downgrade is claimed.

## Historical source and preserved anchors

The original dated checkpoints and their numbers are preserved byte-for-byte in the
[captured document](history/archives/2026-11-17/fetchrpc-v4.md.txt). They are not current release instructions.

<details>
<summary>Legacy section links</summary>

<a id="fetchrpc-v4-contract"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1) — fetchrpc-v4-contract

<a id="capsid-review-phase-107"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L8) — capsid-review-phase-107

<a id="第107轮2026-11-07sdk70与分支修复合入继续主线与干净构建验收"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L10) — 第107轮2026-11-07sdk70与分支修复合入继续主线与干净构建验收

<a id="capsid-review-phase-106"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L31) — capsid-review-phase-106

<a id="第106轮2026-11-07私有上下文与未读gzip分支验证进展"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L33) — 第106轮2026-11-07私有上下文与未读gzip分支验证进展

<a id="capsid-review-phase-105"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L47) — capsid-review-phase-105

<a id="第105轮更新2026-11-07"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L49) — 第105轮更新2026-11-07

<a id="capsid-review-phase-104-in-progress"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L59) — capsid-review-phase-104-in-progress

<a id="第104轮进行中2026-10-24主树480个定向组合补齐4处助手额度并定位运行时超时异常"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L61) — 第104轮进行中2026-10-24主树480个定向组合补齐4处助手额度并定位运行时超时异常

<a id="已合入主树4处c-api测试助手归还响应额度"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L67) — 已合入主树4处c-api测试助手归还响应额度

<a id="新发现快照正文在部分发送后超时取消会污染运行时异常状态"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L87) — 新发现快照正文在部分发送后超时取消会污染运行时异常状态

<a id="私有js-v7仍按原范围保留"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L97) — 私有js-v7仍按原范围保留

<a id="当前差异与继续顺序"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L103) — 当前差异与继续顺序

<a id="capsid-review-phase-103"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L112) — capsid-review-phase-103

<a id="第-103-轮2026-10-21byob-参数顺序标准容量拒绝与-abi-自检同步"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L114) — 第-103-轮2026-10-21byob-参数顺序标准容量拒绝与-abi-自检同步

<a id="capsid-review-phase-102"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L143) — capsid-review-phase-102

<a id="第-102-轮2026-10-21大体积读取defaultreader-分配及偏移-eof"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L145) — 第-102-轮2026-10-21大体积读取defaultreader-分配及偏移-eof

<a id="capsid-review-phase-101"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L176) — capsid-review-phase-101

<a id="第-101-轮2026-10-21读取器原型借用float16array-与-abi-测试归属修复"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L178) — 第-101-轮2026-10-21读取器原型借用float16array-与-abi-测试归属修复

<a id="capsid-review-phase-100"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L207) — capsid-review-phase-100

<a id="第-100-轮2026-10-21upgrade-回退-http-响应体的原生-byob-基础矩阵"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L209) — 第-100-轮2026-10-21upgrade-回退-http-响应体的原生-byob-基础矩阵

<a id="capsid-review-phase-99"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L238) — capsid-review-phase-99

<a id="第-99-轮2026-10-20原生-body-的直接-defaultreader-构造路径"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L240) — 第-99-轮2026-10-20原生-body-的直接-defaultreader-构造路径

<a id="capsid-review-phase-98"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L267) — capsid-review-phase-98

<a id="第-98-轮2026-10-20优化构建中的真实分配探针与完整-spawn-异常遍历"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L269) — 第-98-轮2026-10-20优化构建中的真实分配探针与完整-spawn-异常遍历

<a id="capsid-review-phase-97"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L292) — capsid-review-phase-97

<a id="第-97-轮2026-10-20自定义配置v4-初始化与真实资源回收"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L294) — 第-97-轮2026-10-20自定义配置v4-初始化与真实资源回收

<a id="capsid-review-phase-96"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L325) — capsid-review-phase-96

<a id="第-96-轮2026-10-20默认-abi-分配失败资源回收和测试覆盖修正"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L327) — 第-96-轮2026-10-20默认-abi-分配失败资源回收和测试覆盖修正

<a id="capsid-review-phase-95"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L339) — capsid-review-phase-95

<a id="capsid-review-phase-94"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L353) — capsid-review-phase-94

<a id="第-94-轮2026-10-20默认编码流栈修复完整三配置基线与新增差异"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L355) — 第-94-轮2026-10-20默认编码流栈修复完整三配置基线与新增差异

<a id="capsid-review-phase-93"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L374) — capsid-review-phase-93

<a id="第-93-轮2026-10-20五栈-qps-控制器与归档已接通"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L376) — 第-93-轮2026-10-20五栈-qps-控制器与归档已接通

<a id="capsid-review-phase-92"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L387) — capsid-review-phase-92

<a id="第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L389) — 第-92-轮状态2026-10-20qps-负载正确性原始证据修复完整生产器继续

<a id="capsid-review-phase-91"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L398) — capsid-review-phase-91

<a id="第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L399) — 第-91-轮状态2026-10-20编译器发布兼容修复完整目标继续

<a id="capsid-review-phase-90"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L408) — capsid-review-phase-90

<a id="第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L409) — 第-90-轮状态2026-10-20cold-控制器归档接通完整目标继续

<a id="capsid-review-phase-89"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L422) — capsid-review-phase-89

<a id="第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L423) — 第-89-轮状态2026-10-20cold-探针已修整车道与强兼容目标未完成

<a id="capsid-review-phase-88"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L434) — capsid-review-phase-88

<a id="第-88-轮状态2026-10-20密度控制与档案生产器已接通"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L435) — 第-88-轮状态2026-10-20密度控制与档案生产器已接通

<a id="capsid-review-phase-87"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L446) — capsid-review-phase-87

<a id="第-87-轮状态2026-10-20修正性能采集与网页证据边界"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L447) — 第-87-轮状态2026-10-20修正性能采集与网页证据边界

<a id="capsid-review-phase-86"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L458) — capsid-review-phase-86

<a id="第-86-轮状态2026-10-20完成-windows-流栈压力的受控构建对照"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L459) — 第-86-轮状态2026-10-20完成-windows-流栈压力的受控构建对照

<a id="capsid-review-phase-85"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L470) — capsid-review-phase-85

<a id="第-85-轮状态2026-10-20新增-30-项转换与生命周期场景已完成专项闭环"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L471) — 第-85-轮状态2026-10-20新增-30-项转换与生命周期场景已完成专项闭环

<a id="capsid-review-phase-84"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L484) — capsid-review-phase-84

<a id="第-84-轮状态2026-10-20新增-fetch-边界已完成专项闭环"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L485) — 第-84-轮状态2026-10-20新增-fetch-边界已完成专项闭环

<a id="capsid-review-phase-83"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L496) — capsid-review-phase-83

<a id="第-83-轮原生代理复用既有通道额度和来源协议2026-10-20"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L497) — 第-83-轮原生代理复用既有通道额度和来源协议2026-10-20

<a id="capsid-review-phase-82"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L504) — capsid-review-phase-82

<a id="第-82-轮状态2026-10-20managed-更新保留本地连接来源修复-windows-增量缓存"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L505) — 第-82-轮状态2026-10-20managed-更新保留本地连接来源修复-windows-增量缓存

<a id="capsid-review-phase-81"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L516) — capsid-review-phase-81

<a id="第-81-轮状态2026-10-20延后本地关闭缺口已修复"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L517) — 第-81-轮状态2026-10-20延后本地关闭缺口已修复

<a id="capsid-review-phase-80"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L526) — capsid-review-phase-80

<a id="第-80-轮状态2026-10-20异常父连接的等待事件与本地-close"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L527) — 第-80-轮状态2026-10-20异常父连接的等待事件与本地-close

<a id="capsid-review-phase-79"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L534) — capsid-review-phase-79

<a id="第-79-轮状态2026-10-20共享-io-生命周期与完整必测门禁"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L535) — 第-79-轮状态2026-10-20共享-io-生命周期与完整必测门禁

<a id="capsid-review-phase-78"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L544) — capsid-review-phase-78

<a id="第-78-轮状态2026-10-20公开-upgrade-fetch-基本-13-场景已通过"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L545) — 第-78-轮状态2026-10-20公开-upgrade-fetch-基本-13-场景已通过

<a id="capsid-review-phase-77"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L552) — capsid-review-phase-77

<a id="第-77-轮当前进度2026-10-20upgrade-fetch-基础已验证公开链路仍待接通"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L553) — 第-77-轮当前进度2026-10-20upgrade-fetch-基础已验证公开链路仍待接通

<a id="capsid-review-phase-76"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L562) — capsid-review-phase-76

<a id="第-76-轮当前进度2026-10-19集合内建方法fetch-请求头与当前全量回归"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L563) — 第-76-轮当前进度2026-10-19集合内建方法fetch-请求头与当前全量回归

<a id="capsid-review-phase-75"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L576) — capsid-review-phase-75

<a id="第-75-轮当前进度2026-10-19webcrypto-digesthmac-与全量回归闭环"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L577) — 第-75-轮当前进度2026-10-19webcrypto-digesthmac-与全量回归闭环

<a id="fetchrpc-v4-contract-1"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L588) — fetchrpc-v4-contract-1

<a id="capsid-review-phase-74"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L591) — capsid-review-phase-74

<a id="第-74-轮当前进度2026-10-19超时-abort-桥恢复请求上下文"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L592) — 第-74-轮当前进度2026-10-19超时-abort-桥恢复请求上下文

<a id="capsid-review-phase-73"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L601) — capsid-review-phase-73

<a id="第-73-轮当前进度2026-10-19fetch-调用者上下文恢复"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L602) — 第-73-轮当前进度2026-10-19fetch-调用者上下文恢复

<a id="capsid-review-phase-72"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L611) — capsid-review-phase-72

<a id="第-72-轮当前进度2026-10-19fetch-资源回收与取消语义"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L612) — 第-72-轮当前进度2026-10-19fetch-资源回收与取消语义

<a id="capsid-review-phase-71"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L621) — capsid-review-phase-71

<a id="第-71-轮当前进度2026-10-19跨上下文-promise"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L622) — 第-71-轮当前进度2026-10-19跨上下文-promise

<a id="capsid-review-phase-70"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L631) — capsid-review-phase-70

<a id="第-70-轮当前进度2026-10-19异步回调与执行预算"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L632) — 第-70-轮当前进度2026-10-19异步回调与执行预算

<a id="capsid-review-phase-69"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L641) — capsid-review-phase-69

<a id="第-69-轮当前进度2026-10-19连接定时器与-promise-生命周期"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L642) — 第-69-轮当前进度2026-10-19连接定时器与-promise-生命周期

<a id="capsid-review-phase-68"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L675) — capsid-review-phase-68

<a id="第-68-轮当前进度2026-10-19原生异常重建与内建字符串转换"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L676) — 第-68-轮当前进度2026-10-19原生异常重建与内建字符串转换

<a id="capsid-review-phase-67"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L709) — capsid-review-phase-67

<a id="第-67-轮当前进度2026-10-19普通异常对象克隆与引擎类型判断"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L710) — 第-67-轮当前进度2026-10-19普通异常对象克隆与引擎类型判断

<a id="capsid-review-phase-66"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L743) — capsid-review-phase-66

<a id="第-66-轮当前进度2026-10-19异常-getter-与错误监听器的-io"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L744) — 第-66-轮当前进度2026-10-19异常-getter-与错误监听器的-io

<a id="capsid-review-phase-65"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L776) — capsid-review-phase-65

<a id="current-checkpoint-handler15b-synchronous-exception-snapshots"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L777) — current-checkpoint-handler15b-synchronous-exception-snapshots

<a id="1-framing-and-negotiation"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L819) — 1-framing-and-negotiation

<a id="11-frame-layout-unchanged-from-v3"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L821) — 11-frame-layout-unchanged-from-v3

<a id="12-hello-v4-amended-after-the-payload-audit"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L834) — 12-hello-v4-amended-after-the-payload-audit

<a id="121-hello-a2-limits-extension-v41"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L889) — 121-hello-a2-limits-extension-v41

<a id="13-version-negotiation-matrix-deterministic-rejection"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L922) — 13-version-negotiation-matrix-deterministic-rejection

<a id="2-upgrade-requires-cap_upgrade"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L938) — 2-upgrade-requires-cap_upgrade

<a id="3-duplex-channels-requires-cap_duplex_channel"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L988) — 3-duplex-channels-requires-cap_duplex_channel

<a id="31-identity-and-lifecycle"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L990) — 31-identity-and-lifecycle

<a id="32-frame-table-types-are-stable-payloads-below-are-a2-v41"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1012) — 32-frame-table-types-are-stable-payloads-below-are-a2-v41

<a id="32a-optional-fetch_upgrade-metadata-2026-10-20"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1066) — 32a-optional-fetch_upgrade-metadata-2026-10-20

<a id="32b-optional-connection_lifetime-2026-10-20"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1131) — 32b-optional-connection_lifetime-2026-10-20

<a id="33-credits-and-buffering"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1161) — 33-credits-and-buffering

<a id="34-ownership-cleanup-matrix"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1186) — 34-ownership-cleanup-matrix

<a id="35-sequencing-rules"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1203) — 35-sequencing-rules

<a id="351-confirmation-amendment-v42-2026-10-13"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1235) — 351-confirmation-amendment-v42-2026-10-13

<a id="36-close-kinds-and-codes-v41"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1357) — 36-close-kinds-and-codes-v41

<a id="37-error-table-v41-exact-strings"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1391) — 37-error-table-v41-exact-strings

<a id="4-pingpong"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1416) — 4-pingpong

<a id="5-test-vectors"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1421) — 5-test-vectors

<a id="6-settled-design-decisions-plan-d1d6"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1441) — 6-settled-design-decisions-plan-d1d6

<a id="7-abi-surface-informational"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1452) — 7-abi-surface-informational

<a id="executable-contract-corrections-2026-10-13"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1459) — executable-contract-corrections-2026-10-13

<a id="capsid-review-phase-64"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1483) — capsid-review-phase-64

<a id="2026-10-18-transport-drain-and-peer-compatibility"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1484) — 2026-10-18-transport-drain-and-peer-compatibility

<a id="32b-optional-connection-lifetime-2026-10-20"></a>
[Historical section](history/archives/2026-11-17/fetchrpc-v4.md.txt#L1) — 32b-optional-connection-lifetime-2026-10-20

</details>

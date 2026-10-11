# FetchRPC v4 contract (draft for review — freezes before implementation)

Status: **DRAFT**. This contract, its golden vectors
(`tests/fixtures/fetchrpc-v4/vectors.json`), and the conformance
harness (`tests/test_fetchrpc_v4.cc`) freeze together before any v4
implementation starts, as
[ROADMAP.md](../ROADMAP.md#v030-fetchrpc-v4-and-websocket-foundation)
requires. After the freeze, changes need a versioned amendment.

FetchRPC crosses the Host↔Worker process boundary and carries HTTP,
Upgrade, WebSocket, and Host-mediated service traffic. v4 adds
**duplex channels**; v3 remains exactly as shipped in ABI v7.

## 1. Framing and negotiation

### 1.1 Frame layout (unchanged from v3)

```text
offset  size  field
0       4     magic 'C','T','R','W'          (0x43545252, LE)
4       2     version (u16)                  v4 frames: 4
6       2     type   (u16)
8       4     flags  (u32)
12      8     request_id (u64)
20      4     payload_size (u32)             <= 64 KiB
24      …     payload
```

### 1.2 Hello v4

The first frame from each side is `kHello` with `version = 4` and a
fixed payload:

```text
offset  size  field
0       8     compatibility_id_len (u64)   == 71 (sha256: + 64 hex)
8       71    compatibility id
79      4     capabilities (u32 LE bitset)
83      4     reserved (u32, must be 0)
```

Capability bits (bit index → meaning):

| bit | name | effect |
| --- | --- | --- |
| 0 | `CAP_DUPLEX_CHANNEL` | channel frames (§3) are legal |
| 1 | `CAP_UPGRADE` | Upgrade accept/reject ops (§2) are legal |
| 2 | `CAP_HALF_CLOSE` | `kChannelHalfClose` is legal |
| 3..31 | reserved | **must be zero**; a nonzero reserved bit or an unknown capability bit is a fatal protocol error |

Allocation rule: capabilities are a conjunction — a feature is usable
on a connection only when **both** Hellos carry its bit. The side
observant of the mismatch must not emit frames for the unsupported
feature.

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
forwarded to the worker as a normal v3 `RequestHead` **plus** the
frame flag `kFlagRequestUpgrade` (v4 RequestHead-only flag). The
worker answers exactly one of:

- `kResponseHead` with status 101 **and** `kFlagResponseUpgrade`, whose
  payload carries `channel_id` (u32, host-assigned, see §3.1) — accept;
  the HTTP response headers the host writes are the RFC 6455 accept
  handshake (`Sec-WebSocket-Accept`), produced by the host;
- a normal non-101 response — reject; the host answers the upgrade
  with that response and no channel exists.

Any other response to an upgrade-flagged request (e.g. status 101
without `kFlagResponseUpgrade`, or the upgrade flag on a
non-ResponseHead) is a protocol violation (`invalid upgrade response`).

## 3. Duplex channels (requires `CAP_DUPLEX_CHANNEL`)

### 3.1 Identity and lifecycle

- `channel_id` is a **u32 assigned by the host** (the worker never
  mints ids) from a monotonic counter starting at 1; 0 is never a
  channel. The v3 `request_id` namespace is untouched.
- A channel is created by the host (`kChannelOpen` carries the id) or
  requested by the worker (`kChannelOpen` with id 0 = outbound dial
  request; the host answers `kChannelAccept` with the assigned id or
  `kChannelReject`).
- Ownership classes (exactly one per channel, fixed at open):
  `request-bound` (dies with the request), `response-stream-bound`
  (dies when the response stream ends), `tail-bound` (v0.5+), or
  `standalone` (outbound dial). The ownership class is carried in
  `kChannelOpen` (u8) and drives cleanup (§3.4).

### 3.2 Frame table

| type | name | flags | payload | direction |
| --- | --- | --- | --- | --- |
| 15 | `kChannelOpen` | `kFlagChanOutbound` (worker→host dial), else inbound | u32 id (0 = request an id), u8 ownership, u64 max_message_bytes | both |
| 16 | `kChannelAccept` | — | u32 id | both |
| 17 | `kChannelReject` | — | u32 id, u32 reason_code | both |
| 18 | `kChannelData` | `kFlagStart`, `kFlagEnd`, `kFlagChanText` | message bytes (fragmented by start/end like v3 bodies) | both |
| 19 | `kChannelCredit` | — | u32 id, u64 credit | both |
| 20 | `kChannelHalfClose` | — | u32 id | both (requires `CAP_HALF_CLOSE`) |
| 21 | `kChannelClose` | — | u32 id, u16 code, reason bytes (≤123) | both |
| 22 | `kChannelCancel` | — | u32 id | both |

`kChannelClose` maps 1:1 to a WebSocket Close frame; codes and reason
limits follow the workerd dossier (`docs/workerd-websocket-semantics.md`
§4).

### 3.3 Credits and buffering

- Each direction maintains an independent u64 byte credit. Data may be
  sent only when credit ≥ payload; sending without credit is a
  protocol violation (`channel credit underflow`).
- Initial credit (both directions): 64 KiB. Credit is replenished by
  `kChannelCredit` (additive).
- Per-channel receive buffering is capped at 4 MiB (v3
  `kMaxBufferedBytes`); a sender pushing a peer over the cap is
  throttled by credit exhaustion, never by silent drop.
- Message size limit is negotiated per channel in `kChannelOpen`
  (`max_message_bytes`, default 1 MiB, hard cap 64 MiB); a larger
  complete message is a protocol violation (`message size exceeded`).

### 3.4 Ownership cleanup matrix

| event | request-bound | response-stream-bound | tail-bound | standalone |
| --- | --- | --- | --- | --- |
| client disconnect | host sends `kChannelClose(1006-substitute code 1001)` then frees | same | survives until tail ends | host sends Close, frees |
| request timeout | host sends `kChannelClose(1001)`, frees | same | per tail deadline | unaffected |
| worker shutdown (clean) | worker sends Close for every open channel before `kShutdown` | same | same | same |
| worker replacement | host closes channels with code 1001, does not deliver to the old worker | same | same | same |
| peer Close received | deliver Close event, channel freed after both directions quiesce | same | same | same |

Half-close (`kChannelHalfClose`) ends one direction; the channel frees
when both directions are closed or half-closed. For WebSockets,
half-close maps to "stop sending" (the JS API does not expose it in
v0.3.0; it exists on the wire for transport correctness).

### 3.5 Sequencing rules

- `kChannelData`/`Credit`/`HalfClose`/`Close`/`Cancel` referencing an
  unknown, rejected, or already-freed id: protocol violation
  (`unknown channel`).
- Frame order per channel is strict: Open → (Accept) → Data* →
  (HalfClose) → Close. A Data frame after Close, or a second Open for
  the same id: violation (`channel sequence`).
- Fragmentation: a `kFlagStart`-less first data frame or interleaved
  fragments of two messages on one channel: violation
  (`fragmentation`).
- Duplicate `kChannelCredit`/`Close` are idempotent **only** for
  credit (additive by definition); duplicate Close is a violation.

## 4. Ping/pong

Owned entirely by the host transport; no FetchRPC frame exists. The
worker never sees pings (dossier §5).

## 5. Test vectors

Golden encodings and sequences live in
`tests/fixtures/fetchrpc-v4/vectors.json` (schema documented in the
file). The harness `tests/test_fetchrpc_v4.cc` validates the data,
asserts the shipped v3 parser rejects every v4-versioned vector (the
never-mix-silently boundary), and — once the v4 surface lands —
round-trips every vector. Sequences cover: negotiation accept/reject
matrix, upgrade accept and reject, fragmented message, credit
exhaustion and replenish, half-close, simultaneous close, unknown
channel, duplicate close, out-of-order data, and the four ownership
cleanup rows.

## 6. ABI surface (informational; decided at review)

Additive v7 entries expected: `capsid_worker_upgrade_respond(...)` and
channel event pump entries on the existing event stream
(`CAPSID_EVENT_CHANNEL_*`). Escalation to ABI v8 only if embedders
cannot express duplex pumping additively.

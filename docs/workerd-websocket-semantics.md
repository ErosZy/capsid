# workerd WebSocket semantics dossier (v0.3.0 reference)

Behavioral reference for the v0.3.0 WebSocket vertical slice, verified
against workerd commit `6dd2348caee25ee1f93e3b27ed468ad53bfa59a5`
(compatibility date 2026-08-20). Every claim cites the file and the
anchor visible at that commit. Where Capsid intentionally differs, the
deviation is listed in the last section and must be registered in
`docs/conformance.md` when the API ships.

## 1. WebSocketPair

- `new WebSocketPair()` returns a pair object that is **iterable**
  (entries iterator, `web-socket.c++` `WebSocketPair::entries`,
  `WebSocketPair::constructor` around L1416-1428) — the canonical
  worker idiom is `const [client, server] = Object.values(new
  WebSocketPair())`.
- The pair is an in-memory pipe: the **host terminates the client
  side** against the network; the worker only ever holds the server
  side of an inbound connection (`web-socket.h`, DeferredProxy /
  adapter split — the worker-side `WebSocket` wraps a kj pipe end, not
  a socket).
- Sending on one end surfaces a message event on the other end with no
  network in between; the host side performs all RFC 6455 framing.

## 2. Accepting an inbound WebSocket

- An inbound Upgrade is accepted by returning a 101 response whose
  `webSocket` response extension carries the **server** socket, after
  `server.accept()` (`web-socket.h` L234: only one of `accept()` or
  the response extension is allowed; using the socket without either
  is an error).
- `accept(AcceptOptions)` exists; options are hibernation-related and
  out of scope for v0.3.0.
- Calling `accept()` twice throws `InvalidStateError`.

## 3. Events and messaging

- Event surface: `open` (outbound client sockets), `message`,
  `messageerror` (text frame that is not valid UTF-8), `close`, and
  `error` per the `WebSocketEventMap` in `web-socket.h`.
- `message` events deliver `event.data` as `USVString` for text
  frames and `ArrayBuffer` for binary frames.
- `send()` accepts string, ArrayBuffer, ArrayBufferView, Blob;
  queuing is bounded by the runtime — a worker that outruns its
  consumer eventually blocks/drops (backpressure contract restated in
  the FetchRPC v4 credit model).

## 4. Close semantics

- **Code validation** (`web-socket.c++` L870-886):
  - WHATWG/pedantic mode (`pedanticWpt` flag): only `1000` or
    `3000-4999`; anything else throws `DOMInvalidAccessError`.
  - Legacy mode (the default): `1000-4999` **excluding** `1004`,
    `1005`, `1006`, `1015` — the RFC 6455 §7.4.1 codes an endpoint
    must not send.
  - Validation happens before readyState checks (WHATWG `close()`
    step 1).
- **Reason limit** (`websocketCloseReasonByteLimit` flag): the UTF-8
  encoding must be ≤ 123 bytes (125-byte close-frame body minus the
  2-byte code); violation throws `DOMSyntaxError`.
- `1006` is **internal-only**: the runtime synthesizes
  `CloseEvent(1006, "WebSocket disconnected without sending Close
  frame.")` when the connection drops without a Close frame
  (`web-socket.c++` L801), and `1006 / "Failed to establish websocket
  connection"` on establishment failure (L365). A worker can never
  send 1006.

## 5. Transport-level behavior owned by the host

- Ping/pong: answered transparently at the transport; no JS surface.
- Protocol violations (bad opcode, invalid UTF-8 in a text frame,
  unmasked client frames) surface as `messageerror` or an abnormal
  close — the worker never sees raw frame errors.
- Close handshake completion determines `CloseEvent.wasClean`.

## 6. Outbound WebSockets

- `new WebSocket(url, protocols)` from the worker dials out through
  the runtime under egress control; `open` fires on establishment,
  `close(1006, …)` on failure.
- The Capsid equivalent must route the dial through the host egress
  policy (`src/egress_policy.h`) — the worker never opens sockets
  itself.

## 7. Capsid deviations (planned)

- Hibernation APIs (`serializeAttachment`, hibernation-accept
  options) are not part of v0.3.0.
- `WebSocketPair` client end is only meaningful to the host; exposing
  a worker-usable client end (same-isolate echo) is deferred.
- Compression extensions are out of scope.

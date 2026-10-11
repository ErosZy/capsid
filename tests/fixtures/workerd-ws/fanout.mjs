// Absolute contract: one server socket joined to a room; a message sent
// by any member is delivered to every OTHER member exactly once, and the
// sender never receives its own message back.
const room = new Set();
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "fanout",
        expects: [
          "two upgrades both yield 101",
          "message from A delivered to B once, not to A",
          "message from B delivered to A once, not to B"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    room.add(server);
    server.addEventListener("message", (event) => {
      for (const peer of room) {
        if (peer !== server) peer.send(event.data);
      }
    });
    server.addEventListener("close", () => room.delete(server));
    return new Response(null, { status: 101, webSocket: client });
  }
};

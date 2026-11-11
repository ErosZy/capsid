// Absolute contract: abrupt transport death mid-stream — the client
// vanishes (TCP reset / hard close) while a large transfer is in
// flight; the worker's socket terminates (close event observed) and
// the isolate does not hang or crash. Any bytes already accepted are
// not owed to anyone after the drop.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "disconnect-mid-stream",
        expects: [
          "hard client drop during transfer fires the worker close path",
          "workerd process stays alive and serves a subsequent upgrade"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    server.binaryType = "arraybuffer";
    let closed = false;
    server.addEventListener("close", (event) => {
      closed = true;
    });
    server.addEventListener("message", (event) => {
      // on trigger, stream large frames back-to-back
      const chunk = new Uint8Array(64 * 1024).fill(0x41);
      for (let index = 0; index < 128; index += 1) {
        server.send(chunk);
      }
      server.send("burst-done");
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};

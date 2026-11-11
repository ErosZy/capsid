// Absolute contract: bounded egress with a slow consumer — the worker
// pushes an 8 MiB burst; the client deliberately stalls its reads and
// then resumes. workerd's outbound queue applies backpressure to the
// worker (send loops do not consume unbounded memory) and every byte
// eventually arrives intact: no loss, no duplication, payload
// verifiable in chunks.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "slow-consumer",
        client: "raw-socket-only",
        expects: [
          "stalled reads do not lose or duplicate payload",
          "all 8 MiB arrive intact after reads resume",
          "workerd stays responsive to a subsequent request"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    server.binaryType = "arraybuffer";
    server.addEventListener("message", (event) => {
      const chunk = new Uint8Array(64 * 1024).fill(0x5a);
      for (let index = 0; index < 128; index += 1) {
        server.send(chunk);
      }
      server.send("slow-burst-done");
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};

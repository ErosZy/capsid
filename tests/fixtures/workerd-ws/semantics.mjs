// Read worker-side values directly; browser/client defaults are separate.
export default {
  fetch(request) {
    const url = new URL(request.url);
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    if (url.pathname === "/selftest") {
      const defaultBinaryType = server.binaryType;
      const keys = Object.keys(pair);
      const iterableLength = [...pair].length;
      let beforeAcceptError = null;
      try { server.send("too-early"); } catch (error) { beforeAcceptError = error.name; }
      server.accept({ allowHalfOpen: false });
      let repeatedAcceptError = null;
      try { server.accept(); } catch (error) { repeatedAcceptError = error.name; }
      const readyState = server.readyState;
      server.binaryType = "not-a-binary-type";
      const invalidBinaryType = server.binaryType;
      server.close(1000);
      return Response.json({ defaultBinaryType, keys, iterableLength, beforeAcceptError,
        repeatedAcceptError, readyState, invalidBinaryType });
    }
    server.accept({ allowHalfOpen: false });
    server.addEventListener("message", (event) => {
      server.send(JSON.stringify({ binaryType: server.binaryType,
        dataType: event.data.constructor.name }));
      const bytes = new Uint8Array([1, 2, 3]);
      server.send(bytes.subarray(1));
      bytes[1] = 99; // send must copy before the queued write
      server.send(new Blob([new Uint8Array([4, 5])]));
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};

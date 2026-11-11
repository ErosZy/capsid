// Absolute contract (dossier, corrected 2026-10-12): at compat date
// 2026-08-20 workerd REFUSES cross-request I/O on inbound server
// sockets — holding a peer's server socket and calling send() from a
// different request's handler throws
// "Cannot perform I/O on behalf of a different request." (I/O type:
// Native). There is no implicit room/broadcast at this layer; the
// sender itself receives the failure report, the peer receives
// nothing. The driver records actual instance identities before comparing
// request ownership, so a pool's connection distribution cannot fake fanout.
const room = new Map();
const held = new Map();
let instance;
const controlPrefix = '@capsid-fanout:';
export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.endsWith('/healthy')) return new Response('healthy');
    if (url.pathname === "/selftest") {
      return Response.json({
        scenario: "fanout",
        expects: [
          "both upgrades yield 101",
          "cross-request peer.send() throws the different-request I/O error",
          "the peer socket receives nothing",
          "the sender receives the failure report and the sockets stay usable"
        ]
      });
    }
    const [client, server] = Object.values(new WebSocketPair());
    instance ??= crypto.randomUUID();
    const connection = crypto.randomUUID();
    server.accept();
    server.binaryType = "arraybuffer";
    room.set(server, connection);
    server.addEventListener("message", async (event) => {
      if (typeof event.data === 'string' && event.data.startsWith(controlPrefix)) {
        const command = JSON.parse(event.data.slice(controlPrefix.length));
        const response = {op: command.op, challenge: command.challenge, instance, connection};
        if (command.op === 'identity' || command.op === 'stats') {
          response.members = [...room.values()];
        } else if (command.op === 'held') {
          response.held = [...held.keys()];
        } else if (command.op === 'hold') {
          await new Promise(resolve => held.set(connection, resolve));
          held.delete(connection);
        } else if (command.op === 'release') {
          response.released = held.has(command.target);
          held.get(command.target)?.();
        } else if (command.op === 'probe') {
          response.operation = command.operation;
          response.attempts = [];
          for (const [peer, peerConnection] of room) {
            if (peer !== server && peerConnection === command.target) {
              try {
                if (command.operation === 'readyState') void peer.readyState;
                else if (command.operation === 'close') peer.close(1000, 'foreign-close');
                else if (command.operation === 'send') peer.send(command.challenge);
                else throw new Error('unknown fanout operation');
                response.attempts.push({connection: peerConnection, outcome: 'delivered'});
              } catch (error) {
                response.attempts.push({connection: peerConnection, outcome: 'blocked',
                  errorName: error.name, message: error.message});
              }
            }
          }
        } else if (command.op !== 'echo') {
          throw new Error('unknown fanout control');
        }
        server.send(JSON.stringify(response));
        return;
      }
      for (const peer of room.keys()) {
        if (peer !== server) {
          try {
            peer.send(event.data);
            server.send("fanout:delivered");
          } catch (error) {
            server.send("fanout:blocked:" +
              (error && error.message ? error.message : String(error)));
          }
        }
      }
    });
    server.addEventListener("close", () => {
      room.delete(server);
      held.get(connection)?.();
      held.delete(connection);
    });
    return new Response(null, { status: 101, webSocket: client });
  }
};

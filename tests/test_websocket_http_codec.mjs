import assert from "node:assert/strict";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import os from "node:os";
import { randomUUID } from "node:crypto";
import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import { startCapsid, stopCapsid } from "./helpers/ws_host_backend.mjs";
import { responseSession } from "./helpers/ws_response_client.mjs";
import { root, readLock, verifyBinary, verifyClient, fileDigest } from "../tools/workerd-reference.mjs";
import { httpCodecCasesForShard, httpCodecInputPaths, validateHttpCodecReport } from "../tools/workerd-http-codec-contract.mjs";
import { httpCodecPlan, httpCodecReply } from "../tools/workerd-http-codec-input.mjs";
const args = /* @__PURE__ */ new Map();
for (let index = 2; index < process.argv.length; index += 2) {
  assert.ok(["--reference-from-env", "--host", "--worker", "--mode", "--report", "--shard"].includes(process.argv[index]), "known HTTP codec option");
  assert.ok(!args.has(process.argv[index]) && process.argv[index + 1], "unique valued HTTP codec option");
  args.set(process.argv[index], process.argv[index + 1]);
}
const referencePath = args.has("--reference-from-env") ? process.env.CAPSID_WORKERD_REF : void 0;
const reference = referencePath ? path.resolve(referencePath) : void 0;
if (args.has("--reference-from-env")) {
  assert.equal(args.get("--reference-from-env"), "1");
  assert.ok(reference, "CAPSID_WORKERD_REF required");
  for (const key of ["--host", "--worker", "--mode"]) assert.ok(!args.has(key), "no product alias for reference");
}
const host = args.has("--host") ? path.resolve(args.get("--host")) : void 0;
const worker = args.has("--worker") ? path.resolve(args.get("--worker")) : void 0;
const mode = reference ? "reference" : args.get("--mode");
assert.ok(reference || host && worker && ["single-worker", "static-pool", "managed"].includes(mode));
const shard = args.get("--shard");
const cases = httpCodecCasesForShard(shard);
const lock = readLock();
verifyClient(lock);
if (reference) verifyBinary(reference, lock);
let fixture;
const inputs = Object.fromEntries(Object.entries(httpCodecInputPaths).map(([key, name]) => [key, fileDigest(path.join(root, name))]));
const binaries = { host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker) };
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const report = {
  schema: "capsid-http-codec-v1",
  complete: false,
  shard,
  mode,
  runId,
  referenceLock: lock,
  client: { node: process.version, undici: process.versions.undici },
  inputs,
  binaries,
  cases: []
};
const destination = path.resolve(args.get("--report") ?? (process.env.CAPSID_WS_REPORT_DIR ? path.join(process.env.CAPSID_WS_REPORT_DIR, "http-codec-" + shard + "-" + mode + ".json") : path.join(os.tmpdir(), "capsid-http-codec-" + shard + "-" + runId + "-" + mode + ".json")));
assert.ok(!fs.existsSync(destination), "preserve prior HTTP codec evidence");
function save() {
  fs.writeFileSync(destination, JSON.stringify(report, null, 2) + "\n");
}
save();
async function observeUntil(predicate, milliseconds = 3e3) {
  const deadline = performance.now() + milliseconds;
  while (!predicate() && performance.now() < deadline) await delay(5);
  return predicate();
}
async function startCodecUpstream(id, challenge) {
  const plan = httpCodecPlan(id), reply = httpCodecReply(id, challenge);
  const sockets = /* @__PURE__ */ new Map(), records = [];
  const listener = net.createServer({ allowHalfOpen: true }, (socket) => {
    const item = {
      request: null,
      rawRequest: "",
      bytes: 0,
      peerEnded: false,
      peerReset: false,
      fixtureEnded: false,
      fixtureDestroyed: false,
      events: [],
      localAddress: socket.localAddress,
      localPort: socket.localPort,
      sent: null
    };
    records.push(item);
    sockets.set(socket, item);
    let input = Buffer.alloc(0);
    socket.on("error", (error) => {
      if (error.code === "ECONNRESET") item.peerReset = true;
      else item.error = error.code;
    });
    socket.on("end", () => {
      item.peerEnded = true;
      item.events.push("peer-ended");
    });
    socket.on("close", () => sockets.delete(socket));
    socket.on("data", (chunk) => {
      input = Buffer.concat([input, chunk]);
      item.bytes += chunk.length;
      item.rawRequest = input.toString("base64");
      if (item.request !== null) return;
      const end = input.indexOf("\r\n\r\n");
      if (end < 0) return;
      item.request = input.subarray(0, end).toString();
      item.events.push("request");
      item.sent = reply.packet.toString("base64");
      item.held = false;
      if (plan.fragment) {
        item.packets = [reply.head.toString("base64")];
        item.events.push("headers");
        socket.write(reply.head);
        let index = 0;
        const emit = () => {
          if (socket.destroyed) return;
          const part = reply.parts[index];
          item.packets.push(part.toString("base64"));
          item.events.push("part/" + index + "/" + part.length);
          socket.write(part);
          ++index;
          if (index === reply.parts.length) {
            item.fixtureEnded = true;
            item.events.push("sent-complete");
            socket.end();
          } else setTimeout(emit, 30);
        };
        setTimeout(emit, 30);
      } else {
        item.fixtureEnded = true;
        item.events.push("sent-complete");
        socket.end(reply.packet);
      }
    });
  });
  await new Promise((resolve, reject) => {
    listener.once("error", reject);
    listener.listen(0, "127.0.0.1", resolve);
  });
  return { port: listener.address().port, records, snapshot() {
    return structuredClone(records);
  }, async stop() {
    for (const [socket, item] of sockets) {
      item.fixtureDestroyed = true;
      socket.destroy();
    }
    await new Promise((resolve) => listener.close(resolve));
  } };
}
async function startReference(directory) {
  const listener = net.createServer();
  await new Promise((resolve) => listener.listen(0, "127.0.0.1", resolve));
  const port = listener.address().port;
  await new Promise((resolve) => listener.close(resolve));
  fs.copyFileSync(fixture, path.join(directory, "main.mjs"));
  const config = path.join(directory, "config.capnp");
  fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
  const child = spawn(reference, ["serve", config], { stdio: ["ignore", "pipe", "pipe"] });
  let stderr = "";
  child.stderr.on("data", (data) => stderr += data);
  child.stdout.resume();
  const closed = new Promise((resolve) => child.once("close", resolve)), backend = { child, closed, port, stderr: () => stderr };
  try {
    const deadline = performance.now() + 1e4;
    for (; ; ) try {
      const response = await fetch(`http://127.0.0.1:${port}/state`, { signal: AbortSignal.timeout(500) });
      assert.equal(response.status, 200);
      await response.text();
      return backend;
    } catch (error) {
      assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, "HTTP codec reference startup: " + error + stderr);
      await delay(20);
    }
  } catch (error) {
    await stopReference(backend);
    throw error;
  }
}
async function stopReference(backend) {
  let forced = false, timer;
  if (backend.child.exitCode === null && backend.child.signalCode === null) backend.child.kill("SIGTERM");
  try {
    await Promise.race([backend.closed, new Promise((resolve) => {
      timer = setTimeout(() => {
        forced = true;
        backend.child.kill("SIGKILL");
        resolve();
      }, 2e3);
    })]);
    await backend.closed;
  } finally {
    clearTimeout(timer);
  }
  return { forced, code: backend.child.exitCode, signal: backend.child.signalCode };
}
for (const id of cases) {
  const value = { id, challenge: randomUUID() };
  report.cases.push(value);
  save();
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "capsid-http-codec-"));
  let backend, peer, source, readState;
  try {
    fixture = path.join(root, "tests/fixtures/workerd-http-codec-" + httpCodecPlan(id).fixture + ".mjs");
    source = await startCodecUpstream(id, value.challenge);
    value.targets = [{ host: "localhost", port: source.port }];
    const allow = ["localhost:" + source.port];
    backend = reference ? await startReference(directory) : await startCapsid(
      host,
      worker,
      fixture,
      directory,
      mode,
      { permissions: { modules: [], fetch: { allow } }, fetchTargets: allow }
    );
    const prefix = reference ? "" : "/@capsid/orders";
    readState = async () => {
      const response = await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`, { signal: AbortSignal.timeout(4e3) });
      return { status: response.status, text: await response.text() };
    };
    value.initial = await readState();
    const target = `http://localhost:${source.port}/hold?challenge=${value.challenge}&role=a`;
    const query = new URLSearchParams({ kind: id, challenge: value.challenge, role: "a", target });
    peer = responseSession(backend.port, prefix + "/probe?" + query, { observeCloseReply: true, timeoutMs: 12e3 });
    value.upgradea = await peer.headers;
    assert.equal(value.upgradea.status, 101);
    peer.send("go");
    assert.equal(await peer.text(), "started");
    assert.ok(await observeUntil(() => source.records.some((entry) => entry.request)), "actual ordinary HTTP source request");
    value.beforeAction = source.snapshot();
    value.wirea = await peer.complete;
    assert.ok(
      await observeUntil(() => source.records.length > 0 && source.records.every((entry) => entry.peerEnded)),
      "source observes actual client EOF before host shutdown or fixture disposal"
    );
    value.observed = await readState();
    await delay(100);
    value.finalState = await readState();
    value.upstream = source.snapshot();
  } catch (error) {
    value.failure = error.stack;
    if (error.observation) value.failureObservation = error.observation;
    if (readState) try {
      value.failureState = await readState();
    } catch (nested) {
      value.failureStateError = nested.stack;
    }
  } finally {
    peer?.dispose();
    if (backend) {
      try {
        value.shutdown = reference ? await stopReference(backend) : await stopCapsid(backend.child);
      } catch (error) {
        value.cleanupFailure = error.stack;
      }
      value.stderr = backend.stderr();
    }
    if (source) await source.stop();
    save();
  }
}
for (const [key, name] of Object.entries(httpCodecInputPaths))
  assert.equal(fileDigest(path.join(root, name)), inputs[key], "unchanged formal ordinary-fetch input");
assert.equal(fileDigest(reference ?? host), binaries.host);
if (!reference) assert.equal(fileDigest(worker), binaries.worker);
report.complete = report.cases.every((value) => !value.failure && !value.cleanupFailure && value.shutdown?.forced === false);
save();
validateHttpCodecReport(report, { lock, inputs, runId, host: binaries.host, worker: binaries.worker, shard });
console.log(JSON.stringify({ complete: true, mode, shard, cases: cases.length, report: destination }));

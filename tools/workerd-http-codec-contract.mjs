import assert from "node:assert/strict";
import fs from "node:fs";
import { createHash } from "node:crypto";
import { isDeepStrictEqual } from "node:util";
import { httpCodecCases, httpCodecCasesForShard, httpCodecShards, httpCodecPlan, httpCodecReply, httpCodecInputPaths } from "./workerd-http-codec-input.mjs";
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
function fields(value, names, label) {
  assert.ok(value && typeof value === "object" && !Array.isArray(value), label);
  assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label + " complete fields");
}
function rebind(value, fromChallenge, toChallenge, fromPort, toPort) {
  if (typeof value === "string") return value.replaceAll(fromChallenge, toChallenge).replaceAll("http://localhost:" + fromPort, "http://localhost:" + toPort);
  if (Array.isArray(value)) return value.map((child) => rebind(child, fromChallenge, toChallenge, fromPort, toPort));
  if (value && typeof value === "object") return Object.fromEntries(Object.entries(value).map(([key, child]) => [key, rebind(child, fromChallenge, toChallenge, fromPort, toPort)]));
  return value;
}
function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object") return Object.fromEntries(Object.keys(value).sort().map((key) => [key, canonical(value[key])]));
  return value;
}
function normalizedState(sample) {
  return rebind(JSON.parse(sample.observed.text).state, sample.challenge, "CHALLENGE", sample.targets[0].port, "PORT");
}
const archive = JSON.parse(fs.readFileSync(new URL("../tests/fixtures/workerd-http-codec-report.json", import.meta.url), "utf8"));
fields(archive, ["schema", "provenance", "reports"], "codec reference archive");
assert.equal(archive.schema, "capsid-http-codec-reference-archive-v1");
assert.deepEqual(Object.keys(archive.reports), ["core", "edges", "metadata"]);
assert.equal(archive.provenance.length, 6);
const samples = /* @__PURE__ */ new Map();
for (const [group, report] of Object.entries(archive.reports)) {
  assert.equal(report.complete, true);
  assert.equal(report.mode, "reference");
  const ids = httpCodecCases.filter((id) => httpCodecPlan(id).fixture === group);
  assert.deepEqual(report.cases.map((value) => value.id), ids, "frozen independent codec inventory");
  const provenance = archive.provenance.filter((value) => value.group === group);
  assert.deepEqual(provenance.map((value) => value.platform), ["native", "linux"]);
  const serialized = JSON.stringify(canonical(report.cases.map(normalizedState))).replace(/[\u007f-\uffff]/g, (char) => "\\u" + char.charCodeAt(0).toString(16).padStart(4, "0"));
  const digest = createHash("sha256").update(serialized).digest("hex");
  for (const source of provenance) {
    assert.equal(source.normalizedStateSha256, digest, "both pinned actual workerd states retain their full digest");
    assert.deepEqual(source.caseIDs, ids);
    assert.match(source.sha256, /^[0-9a-f]{64}$/);
    assert.equal(source.referenceLock.source_commit, "8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f");
    assert.equal(source.referenceLock.release_tag, "v1.20261010.1");
    assert.equal(source.referenceLock.compatibility_date, "2026-08-20");
    assert.deepEqual(source.referenceLock.compatibility_flags, []);
    assert.equal(source.binaries.host, source.platform === "native" ? "c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a" : "50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd");
    assert.equal(source.binaries.worker, null);
    assert.equal(source.client.node, "v24.14.0");
  }
  for (const sample of report.cases) {
    assert.ok(!samples.has(sample.id));
    samples.set(sample.id, sample);
  }
}
assert.equal(samples.size, 65);
function freeze(value) {
  if (value && typeof value === "object" && !Object.isFrozen(value)) {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}
const expectedCache = /* @__PURE__ */ new Map();
const replyCache = /* @__PURE__ */ new Map(), bodyCache = /* @__PURE__ */ new Map(), readCache = /* @__PURE__ */ new Map();
function replyModel(id, challenge) {
  const key = JSON.stringify([id, challenge]);
  if (replyCache.has(key)) return replyCache.get(key);
  const reply = httpCodecReply(id, challenge), model = freeze({
    packet: reply.packet.toString("base64"),
    head: reply.head.toString("base64"),
    parts: reply.parts.map((part) => ({ wire: part.toString("base64"), length: part.length })),
    payload: Array.from(reply.payload)
  });
  if (replyCache.size >= 512) replyCache.delete(replyCache.keys().next().value);
  replyCache.set(key, model);
  return model;
}
function expectedHttpCodecState(id, challenge, port) {
  const key = JSON.stringify([id, challenge, port]);
  if (expectedCache.has(key)) return expectedCache.get(key);
  const sample = samples.get(id);
  assert.ok(sample, "known codec state");
  const result = freeze(rebind(JSON.parse(sample.observed.text).state, sample.challenge, challenge, sample.targets[0].port, port));
  if (expectedCache.size >= 512) expectedCache.delete(expectedCache.keys().next().value);
  expectedCache.set(key, result);
  return result;
}
function independentBody(state, id) {
  const plan = httpCodecPlan(id), operation = plan.operation ?? "valid";
  const nullBody = plan.method === "HEAD" || [204, 205, 304].includes(plan.status);
  if (nullBody) {
    assert.deepEqual(state.reads, []);
    assert.equal(state.closed, null);
    assert.equal(state.failure, null);
    return;
  }
  let model = bodyCache.get(id);
  if (!model) {
    const plain = Buffer.alloc(plan.bytes);
    if (plan.entropy) {
      let value = 305419896;
      for (let i = 0; i < plain.length; ++i) {
        value ^= value << 13;
        value ^= value >>> 17;
        value ^= value << 5;
        plain[i] = value & 255;
      }
    } else for (let i = 0; i < plain.length; ++i) plain[i] = (i * 29 + 17) % 251;
    let bytes2 = (plan.encoding ?? "gzip") === "gzip" ? Array.from(plain) : replyModel(id, "CHALLENGE").payload, error2 = null;
    if (["header-invalid", "header-crc-invalid", "reserved", "isize-invalid", "crc", "member-second-crc"].includes(operation) || plan.fragment === "crc-late") {
      error2 = { name: "TypeError", message: "Gzip decompression failed." };
      const valid = operation === "member-second-crc" ? plan.split : operation === "crc" ? Math.floor(Math.max(0, plan.bytes - 1) / (plan.view ?? 4096)) * (plan.view ?? 4096) : plan.fragment === "crc-late" ? plan.bytes : 0;
      bytes2 = bytes2.slice(0, valid);
    } else if (["truncated", "member-second-truncated", "trailing-byte", "wire-empty"].includes(operation) || plan.fragment === "truncated-late") {
      error2 = { name: "TypeError", message: "Gzip compressed stream ended prematurely." };
      if (operation === "wire-empty") bytes2 = [];
    }
    model = freeze({ bytes: bytes2, error: error2 });
    bodyCache.set(id, model);
  }
  const { bytes, error } = model;
  assert.deepEqual(state.failure, error, id + " independent decoder outcome");
  let cursor = 0;
  for (const [index, row] of state.reads.entries()) {
    if (row.done) {
      assert.equal(index, state.reads.length - 1);
      assert.equal(error, null);
      const expected = plan.view ? { inputAfter: 0, done: true, type: "Uint8Array", offset: 0, length: 0, capacity: plan.view + 16, bytes: [], backing: Array(plan.view + 16).fill(165) } : { ...plan.fixture === "core" ? {} : { inputAfter: null }, done: true, type: null, offset: null, length: null, capacity: null, bytes: null, backing: null };
      assert.deepEqual(row, expected, id + " independent EOF");
      continue;
    }
    const capacity = plan.view ? plan.view + 16 : 4096, offset = plan.view ? 8 : 0;
    assert.equal(row.type, "Uint8Array");
    assert.equal(row.offset, offset);
    assert.equal(row.capacity, capacity);
    assert.ok(Number.isInteger(row.length) && row.length > 0 && row.length <= (plan.view ?? 4096));
    assert.ok(cursor + row.length <= bytes.length);
    const modelKey = JSON.stringify([id, cursor, row.length]);
    let expectedRead = readCache.get(modelKey);
    if (!expectedRead) {
      const expectedBytes = bytes.slice(cursor, cursor + row.length), backing = Array(capacity).fill(plan.view ? 165 : 0);
      backing.splice(offset, row.length, ...expectedBytes);
      expectedRead = freeze({ ...plan.fixture === "core" ? {} : { inputAfter: plan.view ? 0 : null }, done: false, type: "Uint8Array", offset, length: row.length, capacity, bytes: expectedBytes, backing });
      if (readCache.size >= 4096) readCache.delete(readCache.keys().next().value);
      readCache.set(modelKey, expectedRead);
    }
    assert.ok(isDeepStrictEqual(row, expectedRead), id + " independent complete row, payload, backing and detachment");
    cursor += row.length;
  }
  assert.equal(cursor, bytes.length, id + " no missing or fabricated payload bytes");
  assert.equal(state.closedStable, true);
  assert.deepEqual(state.closed, error ? { error } : { fulfilled: true });
  if (!error) assert.equal(state.reads.at(-1)?.done, true, "successful body has exact terminal read");
  if (Object.hasOwn(state, "closedAfterRead")) assert.deepEqual(state.closedAfterRead, state.reads.map((row) => row.done ? { fulfilled: true } : null));
  assert.equal(state.after.bodyUsed, true);
  assert.equal(state.after.locked, false);
}
function wire(value, upgrade, terminal) {
  fields(value, ["status", "rawHeader", "requestKey", "rawTail", "frames", "peerTerminated", "transportError", "completion", "closeReplySent"], "codec wire");
  assert.equal(value.status, 101);
  assert.equal(Buffer.from(value.requestKey, "base64").length, 16);
  assert.equal(Buffer.from(value.requestKey, "base64").toString("base64"), value.requestKey);
  const lines = value.rawHeader.split("\r\n");
  assert.equal(lines.shift(), "HTTP/1.1 101 Switching Protocols");
  const headers = lines.map((line) => {
    const colon = line.indexOf(":");
    assert.ok(colon > 0);
    return [line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()];
  }).sort();
  const accept = createHash("sha1").update(value.requestKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
  assert.deepEqual(headers, [["connection", "Upgrade"], ["sec-websocket-accept", accept], ["upgrade", "websocket"]]);
  if (!terminal) {
    assert.deepEqual(value, { ...value, rawTail: "", frames: [], peerTerminated: false, transportError: null, completion: null, closeReplySent: false });
    return;
  }
  assert.equal(value.rawHeader, upgrade.rawHeader);
  assert.equal(value.requestKey, upgrade.requestKey);
  const frames = [...["started", "done"].map((text) => ({ opcode: 1, payload: Buffer.from(text).toString("base64") })), { opcode: 8, payload: Buffer.from([3, 232, 100, 111, 110, 101]).toString("base64") }];
  assert.deepEqual(value.frames, frames);
  assert.equal(value.rawTail, Buffer.concat(frames.map((frame) => {
    const bytes = Buffer.from(frame.payload, "base64");
    return Buffer.concat([Buffer.from([128 | frame.opcode, bytes.length]), bytes]);
  })).toString("base64"));
  assert.equal(value.peerTerminated, true);
  assert.equal(value.closeReplySent, true);
  assert.ok(["peer-ended", "peer-reset"].includes(value.completion));
  assert.equal(value.transportError, value.completion === "peer-reset" ? "ECONNRESET" : null);
}
export function isHttpCodecPeerTerminal(entry, id) {
  if (entry.peerEnded === true && entry.peerReset === false) return true;
  // The pinned Windows reference can reset after receiving the complete bad
  // CRC response. This is actual remote termination, observed before cleanup.
  return id === "codec-supp-fragment-crc-late" && entry.peerEnded === false &&
    entry.peerReset === true && entry.fixtureEnded === true && entry.fixtureDestroyed === false;
}
export function assertHttpCodecSourceEntries(entries, value, terminal) {
  assert.equal(entries.length, 1);
  const entry = entries[0], plan = httpCodecPlan(value.id), reply = replyModel(value.id, value.challenge), port = value.targets[0].port;
  fields(entry, ["request", "rawRequest", "bytes", "peerEnded", "peerReset", "fixtureEnded", "fixtureDestroyed", "events", "localAddress", "localPort", "sent", "held", ...plan.fragment ? ["packets"] : []], "codec source");
  const lines = entry.request.split("\r\n");
  assert.equal(lines.shift(), (plan.method ?? "GET") + " /hold?challenge=" + value.challenge + "&role=a HTTP/1.1");
  const headers = lines.map((line) => {
    const colon = line.indexOf(":");
    assert.ok(colon > 0);
    return [line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()];
  }).sort();
  assert.deepEqual(headers, [["host", "localhost:" + port]]);
  assert.equal(entry.rawRequest, Buffer.from(entry.request + "\r\n\r\n").toString("base64"));
  assert.equal(entry.bytes, Buffer.byteLength(entry.request + "\r\n\r\n"));
  assert.equal(entry.localAddress, "127.0.0.1");
  assert.equal(entry.localPort, port);
  assert.equal(entry.sent, reply.packet);
  assert.equal(entry.held, false);
  assert.equal(entry.fixtureDestroyed, false);
  assert.equal(typeof entry.peerReset, "boolean");
  if (entry.peerReset) assert.ok(isHttpCodecPeerTerminal(entry, value.id), "only the qualified complete CRC reset is a source terminal");
  assert.equal(typeof entry.peerEnded, "boolean");
  assert.equal(typeof entry.fixtureEnded, "boolean");
  if (terminal) {
    assert.equal(isHttpCodecPeerTerminal(entry, value.id), true);
    assert.equal(entry.fixtureEnded, true);
  }
  const events = ["request"];
  if (plan.fragment) {
    assert.ok(Array.isArray(entry.packets));
    const packets = [reply.head, ...reply.parts.map((part) => part.wire)];
    assert.ok(entry.packets.length >= 1 && entry.packets.length <= packets.length);
    assert.deepEqual(entry.packets, packets.slice(0, entry.packets.length));
    if (terminal) assert.equal(entry.packets.length, packets.length);
    events.push("headers", ...reply.parts.slice(0, entry.packets.length - 1).map((part, index) => "part/" + index + "/" + part.length));
    if (entry.fixtureEnded) {
      assert.equal(entry.packets.length, packets.length);
      events.push("sent-complete");
    }
  } else {
    assert.equal(entry.fixtureEnded, true);
    events.push("sent-complete");
  }
  if (plan.fragment === "crc-late") {
    assert.deepEqual(entry.events.filter((event) => event !== "peer-ended"), events, "complete fragmented source history");
    assert.equal(entry.events.filter((event) => event === "peer-ended").length, entry.peerEnded ? 1 : 0);
    if (entry.peerEnded) assert.ok(entry.events.indexOf("peer-ended") > entry.events.indexOf("part/3/1") && entry.events.includes("part/3/1"));
  } else {
    if (entry.peerEnded) events.push("peer-ended");
    assert.deepEqual(entry.events, events, "exact complete causal source history");
  }
}
function assertHttpCodecCase(value, mode) {
  fields(value, ["id", "challenge", "targets", "initial", "upgradea", "beforeAction", "wirea", "observed", "finalState", "upstream", "shutdown", "stderr"], "codec case");
  assert.ok(httpCodecCases.includes(value.id));
  assert.match(value.challenge, uuid);
  assert.equal(value.targets.length, 1);
  fields(value.targets[0], ["host", "port"], "codec target");
  const { host, port } = value.targets[0];
  assert.equal(host, "localhost");
  assert.ok(Number.isInteger(port) && port > 0 && port <= 65535);
  fields(value.initial, ["status", "text"], "initial codec observation");
  assert.equal(value.initial.status, 200);
  const initial = JSON.parse(value.initial.text);
  fields(initial, ["realm", "state"], "initial codec realm");
  assert.match(initial.realm, uuid);
  assert.equal(initial.state, null);
  const expected = expectedHttpCodecState(value.id, value.challenge, port);
  for (const key of ["observed", "finalState"]) {
    fields(value[key], ["status", "text"], key);
    assert.equal(value[key].status, 200);
    const actual = JSON.parse(value[key].text);
    assert.ok(isDeepStrictEqual(actual, { realm: initial.realm, state: expected }), value.id + " full typed " + key + " state");
    independentBody(actual.state, value.id);
  }
  wire(value.upgradea, null, false);
  wire(value.wirea, value.upgradea, true);
  assertHttpCodecSourceEntries(value.beforeAction, value, false);
  assertHttpCodecSourceEntries(value.upstream, value, true);
  const before = value.beforeAction[0], after = value.upstream[0];
  assert.deepEqual(after.events.slice(0, before.events.length), before.events);
  if (before.packets) assert.deepEqual(after.packets.slice(0, before.packets.length), before.packets);
  for (const [key, item] of Object.entries(before)) if (!["events", "packets", "peerEnded", "peerReset", "fixtureEnded"].includes(key)) assert.deepEqual(after[key], item, "retained source identity " + key);
  if (before.peerReset) assert.equal(after.peerReset, true, "an observed reset cannot disappear");
  assert.equal(typeof value.stderr, "string");
  assert.doesNotMatch(value.stderr, /ERROR: (?:AddressSanitizer|LeakSanitizer)|WARNING: ThreadSanitizer|runtime error:|TOKEN LEAK|POISON EXIT/);
  fields(value.shutdown, ["code", "signal", "forced"], "codec shutdown");
  assert.equal(value.shutdown.forced, false);
  if (mode === "reference") assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null || value.shutdown.code === null && value.shutdown.signal === "SIGTERM");
  else assert.deepEqual(value.shutdown, { code: 0, signal: null, forced: false });
  return expectedHttpCodecState(value.id, "CHALLENGE", "PORT");
}
function validateHttpCodecReport(report, expected) {
  fields(report, ["schema", "complete", "shard", "mode", "runId", "referenceLock", "client", "inputs", "binaries", "cases"], "codec report");
  assert.equal(report.schema, "capsid-http-codec-v1");
  assert.equal(report.complete, true);
  assert.equal(report.shard, expected.shard);
  const cases = httpCodecCasesForShard(report.shard);
  assert.equal(report.runId, expected.runId);
  assert.ok(typeof report.runId === "string" && report.runId.length > 0);
  const backends = expected.lock.release_asset === "workerd-linux-64.gz" ? ["single-worker", "static-pool", "managed"] : ["single-worker", "static-pool"];
  assert.ok(["reference", ...backends].includes(report.mode));
  assert.deepEqual(report.referenceLock, expected.lock);
  assert.deepEqual(report.inputs, expected.inputs);
  fields(report.inputs, Object.keys(httpCodecInputPaths), "codec input identities");
  for (const hash of Object.values(report.inputs)) assert.match(hash, /^[0-9a-f]{64}$/);
  assert.deepEqual(report.client, { node: expected.lock.client.node_version, undici: expected.lock.client.undici_version });
  fields(report.binaries, ["host", "worker"], "codec executables");
  assert.equal(report.binaries.host, report.mode === "reference" ? expected.lock.binary_sha256 : expected.host);
  assert.equal(report.binaries.worker, report.mode === "reference" ? null : expected.worker);
  assert.deepEqual(report.cases.map((value) => value.id), cases);
  for (const key of ["challenge"]) assert.equal(new Set(report.cases.map((value) => value[key])).size, cases.length);
  const realms = report.cases.map((value) => JSON.parse(value.initial.text).realm), keys = report.cases.map((value) => value.upgradea.requestKey);
  assert.equal(new Set(realms).size, cases.length);
  assert.equal(new Set(keys).size, cases.length);
  return report.cases.map((value) => assertHttpCodecCase(value, report.mode));
}
function compareHttpCodecReports(references, candidates, expected) {
  assert.deepEqual(references.map((report) => report.shard), httpCodecShards);
  assert.ok(references.every((report) => report.mode === "reference"));
  const modes = expected.lock.release_asset === "workerd-linux-64.gz" ? ["single-worker", "static-pool", "managed"] : ["single-worker", "static-pool"];
  assert.deepEqual(candidates.map((value) => value.mode), modes);
  const reference = references.map((report) => validateHttpCodecReport(report, { ...expected, shard: report.shard }));
  for (const candidate of candidates) {
    assert.deepEqual(candidate.reports.map((report) => report.shard), httpCodecShards);
    for (const [index, report] of candidate.reports.entries()) {
      assert.equal(report.mode, candidate.mode);
      assert.deepEqual(validateHttpCodecReport(report, { ...expected, shard: report.shard }), reference[index]);
    }
  }
  return { complete: true, casesPerBackend: 65, backends: 1 + modes.length, shards: httpCodecShards.map((shard) => ({ shard, cases: httpCodecCasesForShard(shard).length })) };
}
export {
  assertHttpCodecCase,
  compareHttpCodecReports,
  expectedHttpCodecState,
  httpCodecCases,
  httpCodecCasesForShard,
  httpCodecInputPaths,
  httpCodecShards,
  validateHttpCodecReport
};

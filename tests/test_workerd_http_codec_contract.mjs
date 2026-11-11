import assert from "node:assert/strict";
import { test, after } from "node:test";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { readLock } from "../tools/workerd-reference.mjs";
import { referenceBackendModes } from "../tools/workerd-platform.mjs";
import { readHttpCodecSamples } from "./helpers/ws_http_codec_sample.mjs";
import {
  assertHttpCodecCase,
  expectedHttpCodecState,
  httpCodecShards,
  httpCodecCases,
  httpCodecCasesForShard,
  validateHttpCodecReport,
  compareHttpCodecReports
} from "../tools/workerd-http-codec-contract.mjs";
const lock = readLock();
let rejected = 0;
const reject = (operation, ...message) => {
  ++rejected;
  return assert.throws(operation, ...message);
};
after(() => console.log(JSON.stringify({ httpCodecCases: 65, rejectedMutations: rejected })));
function matrix() {
  const references = readHttpCodecSamples(lock), first = references[0];
  const expected = { lock, inputs: first.inputs, runId: first.runId, host: "a".repeat(64), worker: "b".repeat(64) };
  const candidates = referenceBackendModes(lock).map((mode) => ({ mode, reports: references.map((source) => {
    const report = structuredClone(source);
    report.mode = mode;
    report.binaries = { host: expected.host, worker: expected.worker };
    for (const value of report.cases) value.shutdown = { code: 0, signal: null, forced: false };
    return report;
  }) }));
  return { references, candidates, expected };
}
const check = (value) => compareHttpCodecReports(value.references, value.candidates, value.expected);
test("all 65 frozen HTTP codec cases, both mandatory shards and each platform backend", () => {
  const m = matrix();
  assert.equal(httpCodecCases.length, 65);
  assert.equal(new Set(httpCodecCases).size, 65);
  assert.deepEqual(httpCodecShards.map((shard) => httpCodecCasesForShard(shard).length), [34, 31]);
  assert.deepEqual(check(m), { complete: true, casesPerBackend: 65, backends: 1 + m.candidates.length, shards: [{ shard: "decode", cases: 34 }, { shard: "metadata", cases: 31 }] });
});
for (const shard of httpCodecShards) test(shard + ": reject inventory, provenance, executable and schema substitutions", () => {
  const source = readHttpCodecSamples(lock).find((report) => report.shard === shard);
  const expected = { lock, inputs: source.inputs, runId: source.runId, shard };
  for (const mutate of [
    (value) => value.cases.pop(),
    (value) => value.cases.reverse(),
    (value) => value.cases.push(value.cases[0]),
    (value) => value.cases[1].challenge = value.cases[0].challenge,
    (value) => value.cases[1].initial = value.cases[0].initial,
    (value) => value.cases[1].upgradea.requestKey = value.cases[0].upgradea.requestKey,
    (value) => value.complete = false,
    (value) => value.complete = 1,
    (value) => value.schema = "diagnostic",
    (value) => value.shard = "all",
    (value) => value.mode = "other",
    (value) => value.runId = "other",
    (value) => value.referenceLock.compatibility_flags.push("unqualified"),
    (value) => value.referenceLock.source_commit = "f".repeat(40),
    (value) => value.client.node = "v0.0.0",
    (value) => value.inputs.coreFixture = "f".repeat(64),
    (value) => delete value.inputs.contract,
    (value) => value.inputs.unchecked = "f".repeat(64),
    (value) => value.binaries.host = "f".repeat(64),
    (value) => value.binaries.worker = "f".repeat(64),
    (value) => value.ignored = true,
    (value) => delete value.shard
  ]) {
    const bad = structuredClone(source);
    mutate(bad);
    reject(() => validateHttpCodecReport(bad, expected));
  }
});
for (const sample of readHttpCodecSamples(lock).flatMap((report) => report.cases)) test(sample.id + ": reject payload, backing, detachment, errors, EOF and forced cleanup changes", () => {
  assertHttpCodecCase(sample, "reference");
  const controls = [
    (value) => delete value.observed,
    (value) => value.failure = "hidden",
    (value) => value.unknown = 1,
    (value) => value.targets.push(value.targets[0]),
    (value) => value.targets[0].port = 0,
    (value) => value.initial.status = 201,
    (value) => value.observed.status = 201,
    (value) => value.finalState.status = 201,
    (value) => value.shutdown.forced = true,
    (value) => value.shutdown.code = 7,
    (value) => value.stderr = "ERROR: AddressSanitizer: test",
    (value) => value.wirea.frames.pop(),
    (value) => value.wirea.rawTail = "",
    (value) => value.wirea.peerTerminated = false,
    (value) => value.wirea.closeReplySent = false,
    (value) => value.wirea.completion = "local-disposal",
    (value) => value.wirea.transportError = "ETIMEDOUT",
    (value) => value.upgradea.status = 200,
    (value) => value.upgradea.rawHeader += "\r\nX-Unknown: yes",
    (value) => value.upgradea.rawHeader = value.upgradea.rawHeader.replace("Sec-WebSocket-Accept:", "Invalid-Accept:"),
    (value) => value.upstream.push(value.upstream[0]),
    (value) => value.upstream[0].fixtureDestroyed = true,
    (value) => value.upstream[0].peerEnded = false,
    (value) => value.upstream[0].peerReset = true,
    (value) => value.upstream[0].events.reverse(),
    (value) => value.upstream[0].sent += "AA==",
    (value) => value.upstream[0].rawRequest += "AA==",
    (value) => value.upstream[0].bytes += 1,
    (value) => value.upstream[0].localAddress = "::1",
    (value) => value.upstream[0].localPort += 1,
    (value) => value.upstream[0].request += "\r\nUpgrade: websocket",
    (value) => value.beforeAction[0].sent = "",
    (value) => value.beforeAction[0].fixtureDestroyed = true,
    (value) => delete value.upstream[0].held,
    (value) => delete value.beforeAction[0].held
  ];
  if (sample.upstream[0].packets) controls.push((value) => value.upstream[0].packets.pop(), (value) => value.upstream[0].packets[1] += "AA==");
  for (const mutate of controls) {
    const bad = structuredClone(sample);
    mutate(bad);
    reject(() => assertHttpCodecCase(bad, "reference"));
  }
  const state = JSON.parse(sample.observed.text).state, paths = [];
  function collect(value, path = []) {
    if (Array.isArray(value) && value.length) {
      const indices = value.length > 8 ? [0, 1, Math.floor(value.length / 2), value.length - 2, value.length - 1] : value.keys();
      for (const index of new Set(indices)) collect(value[index], [...path, index]);
    } else if (value && typeof value === "object") {
      for (const [key, child] of Object.entries(value)) collect(child, [...path, key]);
    } else paths.push(path);
  }
  collect(state);
  for (const path of paths) {
    const bad = structuredClone(sample), document = JSON.parse(bad.observed.text);
    let parent = document.state;
    for (const key2 of path.slice(0, -1)) parent = parent[key2];
    const key = path.at(-1), old = parent[key];
    parent[key] = typeof old === "number" ? old === 0 ? false : old === 1 ? true : old + 1 : typeof old === "boolean" ? Number(old) : old === null ? "missing" : Array.isArray(old) ? null : old + "/changed";
    bad.observed.text = bad.finalState.text = JSON.stringify(document);
    reject(() => assertHttpCodecCase(bad, "reference"), sample.id + " " + path.join("."));
  }
  for (const key of Object.keys(state)) {
    const bad = structuredClone(sample), document = JSON.parse(bad.observed.text);
    delete document.state[key];
    bad.observed.text = bad.finalState.text = JSON.stringify(document);
    reject(() => assertHttpCodecCase(bad, "reference"));
  }
});
test("reject equally wrong reference and every candidate backend", () => {
  for (const index of [0, 1]) {
    const m = matrix();
    for (const report of [m.references[index], ...m.candidates.map((group) => group.reports[index])]) {
      const value = report.cases[0], document = JSON.parse(value.observed.text);
      document.state.failure = { name: "Error", message: "shared wrong output" };
      value.observed.text = value.finalState.text = JSON.stringify(document);
    }
    reject(() => check(m));
  }
});
test("reject incomplete, reordered and duplicated backend/shard matrices", () => {
  for (const mutate of [
    (m) => m.references.pop(),
    (m) => m.references.reverse(),
    (m) => m.candidates.pop(),
    (m) => m.candidates.push(m.candidates[0]),
    (m) => m.candidates[0].reports.pop(),
    (m) => m.candidates[0].reports.reverse(),
    (m) => m.candidates[0].reports[0].mode = "reference"
  ]) {
    const m = matrix();
    mutate(m);
    reject(() => check(m));
  }
  for (const shard of [void 0, "", "all", "decode,metadata", "__proto__", "unknown"]) reject(() => httpCodecCasesForShard(shard));
});
test("driver rejects missing shard, case filters, duplicate arguments and product/reference aliases", () => {
  for (const args of [
    [],
    ["--shard", "all"],
    ["--shard", "decode", "--shard", "metadata"],
    ["--kinds", "ordinary-codec-gzip-small"],
    ["--reference-from-env", "2", "--shard", "decode"],
    ["--reference-from-env", "1", "--host", "unused", "--shard", "decode"]
  ]) {
    const result = spawnSync(
      process.execPath,
      [fileURLToPath(new URL("./test_websocket_http_codec.mjs", import.meta.url)), ...args],
      { encoding: "utf8", timeout: 5e3, env: { ...process.env, CAPSID_WORKERD_REF: "unused" } }
    );
    assert.equal(result.status, 1);
    assert.match(result.stderr, /AssertionError/);
    assert.doesNotMatch(result.stderr, /ERR_MODULE_NOT_FOUND|SyntaxError/);
  }
});
test("immutable expected states cannot be poisoned between validations", () => {
  const state = expectedHttpCodecState("codec-supp-raw-byob-65536", "CHALLENGE", "PORT");
  assert.equal(Object.isFrozen(state.reads[0].backing), true);
  assert.throws(() => {
    state.reads[0].backing[0] = 0;
  }, TypeError);
  const sample = readHttpCodecSamples(lock).flatMap((report) => report.cases).find((value) => value.id === "codec-supp-raw-byob-65536");
  assertHttpCodecCase(sample, "reference");
});

import assert from "node:assert/strict";
import { gzipSync as platformGzip, deflateSync } from "node:zlib";
const plans = { "ordinary-codec-gzip-small": { "bytes": 97, "fixture": "core", "encoding": "gzip", "operation": "valid" }, "ordinary-codec-deflate-small": { "bytes": 97, "fixture": "core", "encoding": "deflate", "operation": "valid" }, "ordinary-codec-gzip-large": { "bytes": 8197, "fixture": "core", "encoding": "gzip", "operation": "valid" }, "ordinary-codec-deflate-large": { "bytes": 8197, "fixture": "core", "encoding": "deflate", "operation": "valid" }, "ordinary-codec-gzip-header-invalid": { "bytes": 97, "fixture": "core", "encoding": "gzip", "operation": "header-invalid" }, "codec-edge-crc-0": { "bytes": 0, "operation": "crc", "fixture": "edges" }, "codec-edge-crc-97": { "bytes": 97, "operation": "crc", "fixture": "edges" }, "codec-edge-crc-4095": { "bytes": 4095, "operation": "crc", "fixture": "edges" }, "codec-edge-crc-4096": { "bytes": 4096, "operation": "crc", "fixture": "edges" }, "codec-edge-crc-4097": { "bytes": 4097, "operation": "crc", "fixture": "edges" }, "codec-edge-crc-8192": { "bytes": 8192, "operation": "crc", "fixture": "edges" }, "codec-edge-crc-8197": { "bytes": 8197, "operation": "crc", "fixture": "edges" }, "codec-edge-crc-16389": { "bytes": 16389, "operation": "crc", "fixture": "edges" }, "codec-edge-truncated-0": { "bytes": 0, "operation": "truncated", "fixture": "edges" }, "codec-edge-truncated-97": { "bytes": 97, "operation": "truncated", "fixture": "edges" }, "codec-edge-truncated-4096": { "bytes": 4096, "operation": "truncated", "fixture": "edges" }, "codec-edge-truncated-4097": { "bytes": 4097, "operation": "truncated", "fixture": "edges" }, "codec-edge-truncated-8192": { "bytes": 8192, "operation": "truncated", "fixture": "edges" }, "codec-edge-truncated-8197": { "bytes": 8197, "operation": "truncated", "fixture": "edges" }, "codec-edge-empty": { "bytes": 0, "operation": "valid", "split": 0, "fixture": "edges" }, "codec-edge-wire-empty": { "bytes": 0, "operation": "wire-empty", "split": 0, "fixture": "edges" }, "codec-edge-members": { "bytes": 97, "operation": "members", "split": 49, "fixture": "edges" }, "codec-edge-member-first-empty": { "bytes": 97, "operation": "members", "split": 0, "fixture": "edges" }, "codec-edge-member-last-empty": { "bytes": 97, "operation": "members", "split": 97, "fixture": "edges" }, "codec-edge-members-empty": { "bytes": 0, "operation": "members", "split": 0, "fixture": "edges" }, "codec-edge-member-second-crc": { "bytes": 97, "operation": "member-second-crc", "split": 49, "fixture": "edges" }, "codec-edge-member-second-truncated": { "bytes": 97, "operation": "member-second-truncated", "split": 49, "fixture": "edges" }, "codec-edge-trailing-byte": { "bytes": 97, "operation": "trailing-byte", "split": 0, "fixture": "edges" }, "codec-edge-byob-16-valid": { "bytes": 8197, "operation": "valid", "view": 16, "fixture": "edges" }, "codec-edge-byob-16-crc": { "bytes": 8197, "operation": "crc", "view": 16, "fixture": "edges" }, "codec-edge-byob-64-valid": { "bytes": 8197, "operation": "valid", "view": 64, "fixture": "edges" }, "codec-edge-byob-64-crc": { "bytes": 8197, "operation": "crc", "view": 64, "fixture": "edges" }, "codec-edge-byob-8192-valid": { "bytes": 8197, "operation": "valid", "view": 8192, "fixture": "edges" }, "codec-edge-byob-8192-crc": { "bytes": 8197, "operation": "crc", "view": 8192, "fixture": "edges" }, "codec-supp-upper": { "bytes": 97, "encoding": "GZIP", "fixture": "metadata" }, "codec-supp-alias": { "bytes": 97, "encoding": "x-gzip", "fixture": "metadata" }, "codec-supp-ows": { "bytes": 97, "encoding": "gzip ", "fixture": "metadata" }, "codec-supp-double": { "bytes": 97, "encoding": "gzip, gzip", "fixture": "metadata" }, "codec-supp-head": { "bytes": 97, "method": "HEAD", "fixture": "metadata" }, "codec-supp-status-204": { "bytes": 97, "status": 204, "fixture": "metadata" }, "codec-supp-status-205": { "bytes": 97, "status": 205, "fixture": "metadata" }, "codec-supp-status-304": { "bytes": 97, "status": 304, "fixture": "metadata" }, "codec-supp-extra": { "bytes": 97, "operation": "extra", "fixture": "metadata" }, "codec-supp-name": { "bytes": 97, "operation": "name", "fixture": "metadata" }, "codec-supp-comment": { "bytes": 97, "operation": "comment", "fixture": "metadata" }, "codec-supp-header-crc": { "bytes": 97, "operation": "header-crc", "fixture": "metadata" }, "codec-supp-header-crc-invalid": { "bytes": 97, "operation": "header-crc-invalid", "fixture": "metadata" }, "codec-supp-reserved": { "bytes": 97, "operation": "reserved", "fixture": "metadata" }, "codec-supp-isize-invalid": { "bytes": 97, "operation": "isize-invalid", "fixture": "metadata" }, "codec-supp-fragment-header": { "bytes": 97, "fragment": "header", "fixture": "metadata" }, "codec-supp-fragment-trailer": { "bytes": 97, "fragment": "trailer", "fixture": "metadata" }, "codec-supp-fragment-members": { "bytes": 97, "fragment": "members", "fixture": "metadata" }, "codec-supp-fragment-crc-late": { "bytes": 97, "fragment": "crc-late", "fixture": "metadata" }, "codec-supp-fragment-truncated-late": { "bytes": 97, "fragment": "truncated-late", "fixture": "metadata" }, "codec-supp-byob-members-64": { "bytes": 97, "fragment": "members", "view": 64, "fixture": "metadata" }, "codec-supp-byob-members-8192": { "bytes": 97, "fragment": "members", "view": 8192, "fixture": "metadata" }, "codec-supp-entropy": { "bytes": 16389, "entropy": true, "fixture": "metadata" }, "codec-supp-entropy-byob": { "bytes": 16389, "entropy": true, "view": 8192, "fixture": "metadata" }, "codec-supp-raw-byob-4096": { "bytes": 16389, "entropy": true, "view": 4096, "raw": true, "encoding": "identity", "fixture": "metadata" }, "codec-supp-gzip-byob-4096": { "bytes": 16389, "entropy": true, "view": 4096, "fixture": "metadata" }, "codec-supp-raw-byob-8192": { "bytes": 16389, "entropy": true, "view": 8192, "raw": true, "encoding": "identity", "fixture": "metadata" }, "codec-supp-raw-byob-16384": { "bytes": 16389, "entropy": true, "view": 16384, "raw": true, "encoding": "identity", "fixture": "metadata" }, "codec-supp-gzip-byob-16384": { "bytes": 16389, "entropy": true, "view": 16384, "fixture": "metadata" }, "codec-supp-raw-byob-65536": { "bytes": 16389, "entropy": true, "view": 65536, "raw": true, "encoding": "identity", "fixture": "metadata" }, "codec-supp-gzip-byob-65536": { "bytes": 16389, "entropy": true, "view": 65536, "fixture": "metadata" } };
for (const plan of Object.values(plans)) Object.freeze(plan);
const httpCodecPlans = Object.freeze(plans);
const httpCodecShards = Object.freeze(["decode", "metadata"]);
const inventory = Object.freeze(Object.fromEntries(httpCodecShards.map((shard) => [
  shard,
  Object.freeze(Object.keys(plans).filter((id) => plans[id].fixture === "metadata" === (shard === "metadata")))
])));
const httpCodecCases = Object.freeze(httpCodecShards.flatMap((shard) => inventory[shard]));
function httpCodecCasesForShard(shard) {
  assert.ok(httpCodecShards.includes(shard), "mandatory known HTTP codec shard");
  return inventory[shard];
}
function httpCodecPlan(id) {
  assert.ok(Object.hasOwn(plans, id), "known HTTP codec case");
  return plans[id];
}
const httpCodecInputPaths = Object.freeze({
  runner: "tests/test_websocket_http_codec.mjs",
  coreFixture: "tests/fixtures/workerd-http-codec-core.mjs",
  edgeFixture: "tests/fixtures/workerd-http-codec-edges.mjs",
  metadataFixture: "tests/fixtures/workerd-http-codec-metadata.mjs",
  contract: "tools/workerd-http-codec-contract.mjs",
  sourceModel: "tools/workerd-http-codec-input.mjs",
  referenceArchive: "tests/fixtures/workerd-http-codec-report.json",
  hostBackend: "tests/helpers/ws_host_backend.mjs",
  peer: "tests/helpers/ws_response_client.mjs"
});
function gzip(bytes) {
  const wire = platformGzip(bytes);
  wire[9] = 255;
  return wire;
}
function httpCodecPlainBytes(id) {
  const plan = httpCodecPlan(id), body = Buffer.from(Array.from({ length: plan.bytes }, (_, index) => (index * 29 + 17) % 251));
  if (plan.entropy) {
    let value = 305419896;
    for (let i = 0; i < body.length; i++) {
      value ^= value << 13;
      value ^= value >>> 17;
      value ^= value << 5;
      body[i] = value & 255;
    }
  }
  return body;
}
function httpCodecReply(id, challenge) {
  const plan = httpCodecPlan(id), body = httpCodecPlainBytes(id), operation = plan.operation ?? "valid";
  let payload = plan.encoding === "deflate" ? deflateSync(body) : gzip(body);
  if (operation === "header-invalid") {
    payload = Buffer.from(payload);
    payload[0] ^= 1;
  }
  if (operation === "crc") {
    payload = Buffer.from(payload);
    payload[payload.length - 8] ^= 1;
  }
  if (operation === "truncated") payload = payload.subarray(0, payload.length - 5);
  if (operation === "wire-empty") payload = Buffer.alloc(0);
  if (operation.startsWith("member")) {
    let second = gzip(body.subarray(plan.split));
    if (operation === "member-second-crc") {
      second = Buffer.from(second);
      second[second.length - 8] ^= 1;
    }
    if (operation === "member-second-truncated") second = second.subarray(0, second.length - 5);
    payload = Buffer.concat([gzip(body.subarray(0, plan.split)), second]);
  }
  if (operation === "trailing-byte") payload = Buffer.concat([payload, Buffer.from([1])]);
  if (plan.encoding === "gzip, gzip") payload = gzip(payload);
  if (plan.fragment === "members") payload = Buffer.concat([gzip(body.subarray(0, 49)), gzip(body.subarray(49))]);
  if (plan.fragment === "crc-late" || operation === "isize-invalid") {
    payload = Buffer.from(payload);
    payload[payload.length - (operation === "isize-invalid" ? 4 : 8)] ^= 1;
  }
  if (plan.fragment === "truncated-late") payload = payload.subarray(0, payload.length - 5);
  if (["extra", "name", "comment", "header-crc", "header-crc-invalid", "reserved"].includes(operation)) {
    const fixed = Buffer.from(payload.subarray(0, 10));
    let extra = Buffer.alloc(0);
    if (operation === "extra") {
      fixed[3] |= 4;
      extra = Buffer.from([4, 0, 1, 2, 3, 4]);
    }
    if (operation === "name") {
      fixed[3] |= 8;
      extra = Buffer.from("capsid.txt\0");
    }
    if (operation === "comment") {
      fixed[3] |= 16;
      extra = Buffer.from("capsid codec\0");
    }
    if (operation === "reserved") fixed[3] |= 32;
    if (operation.startsWith("header-crc")) {
      fixed[3] |= 2;
      let crc = 4294967295;
      for (const byte of fixed) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) crc = crc >>> 1 ^ (crc & 1 ? 3988292384 : 0);
      }
      crc = (crc ^ 4294967295) >>> 0;
      extra = Buffer.from([crc & 255, crc >>> 8 & 255]);
      if (operation === "header-crc-invalid") extra[0] ^= 1;
    }
    payload = Buffer.concat([fixed, extra, payload.subarray(10)]);
  }
  if (plan.raw) payload = body;
  const status = plan.status ?? 201, phrase = { 201: "Created", 204: "No Content", 205: "Reset Content", 304: "Not Modified" }[status];
  if ([204, 205, 304].includes(status)) payload = Buffer.alloc(0);
  const headers = [
    "Connection: close",
    "Content-Type: application/octet-stream",
    "X-Probe: " + challenge,
    "Content-Encoding: " + (plan.encoding ?? "gzip"),
    "Content-Length: " + payload.length
  ];
  const head = Buffer.from("HTTP/1.1 " + status + " " + phrase + "\r\n" + headers.join("\r\n") + "\r\n\r\n");
  const wire = plan.method === "HEAD" ? Buffer.alloc(0) : payload, packet = Buffer.concat([head, wire]);
  const cuts = plan.fragment === "header" ? [1, 1, 2, 6, payload.length - 10] : plan.fragment === "members" ? [gzip(body.subarray(0, 49)).length, payload.length - gzip(body.subarray(0, 49)).length] : plan.fragment === "truncated-late" ? [payload.length - 3, 1, 2] : [payload.length - 8, 1, 2, 1, 4];
  const parts = [];
  let offset = 0;
  if (plan.fragment) {
    for (const length of cuts) {
      assert.ok(length > 0);
      parts.push(payload.subarray(offset, offset + length));
      offset += length;
    }
    assert.equal(offset, payload.length);
  }
  return { body, payload, head, wire, packet, parts };
}
export {
  httpCodecCases,
  httpCodecCasesForShard,
  httpCodecInputPaths,
  httpCodecPlainBytes,
  httpCodecPlan,
  httpCodecPlans,
  httpCodecReply,
  httpCodecShards
};

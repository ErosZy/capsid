import fs from "node:fs";
import { httpCodecCasesForShard, httpCodecShards, httpCodecReply } from "../../tools/workerd-http-codec-input.mjs";
import { httpCodecInputPaths } from "../../tools/workerd-http-codec-contract.mjs";
const archive = JSON.parse(fs.readFileSync(new URL("../fixtures/workerd-http-codec-report.json", import.meta.url), "utf8"));
const archivedCases = new Map(Object.values(archive.reports).flatMap((report) => report.cases).map((value) => [value.id, value]));
function readHttpCodecSamples(lock) {
  return httpCodecShards.map((shard) => {
    const cases = httpCodecCasesForShard(shard).map((id) => structuredClone(archivedCases.get(id)));
    for (const value of cases) {
      const reply = httpCodecReply(value.id, value.challenge);
      for (const key of ["beforeAction", "upstream"]) for (const source of value[key]) {
        source.sent = reply.packet.toString("base64");
        if (source.packets) source.packets = [reply.head, ...reply.parts].slice(0, source.packets.length).map((part) => part.toString("base64"));
      }
    }
    return {
      schema: "capsid-http-codec-v1",
      complete: true,
      shard,
      mode: "reference",
      runId: "codec-synthetic-control",
      referenceLock: structuredClone(lock),
      client: { node: lock.client.node_version, undici: lock.client.undici_version },
      inputs: Object.fromEntries(Object.keys(httpCodecInputPaths).map((name, index) => [name, index.toString(16).repeat(64)])),
      binaries: { host: lock.binary_sha256, worker: null },
      cases
    };
  });
}
export {
  readHttpCodecSamples
};

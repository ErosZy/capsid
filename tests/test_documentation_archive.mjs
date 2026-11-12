import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { archivedQualification } from "./documentation_archive.mjs";

const relative = "docs/evidence/v0.3.0/2026-11-12-control/qualification-before.md";
async function fixture(run) {
  const root = await mkdtemp(path.join(os.tmpdir(), "capsid-doc-archive-"));
  try {
    const directory = path.join(root, path.dirname(relative));
    await mkdir(directory, { recursive: true });
    const bytes = Buffer.from("old qualification\r\n[original relative link](evidence/original.json)\r\n");
    const index = { files: { "qualification-before.md": createHash("sha256").update(bytes).digest("hex") } };
    const indexPath = path.join(directory, "index.json");
    await writeFile(indexPath, JSON.stringify(index));
    await run({ root, bytes, index, indexPath });
  } finally {
    assert.ok(path.basename(root).startsWith("capsid-doc-archive-"));
    await rm(root, { recursive: true, force: true });
  }
}
test("recognizes only exact inventoried archival bytes", () => fixture(async ({ root, bytes }) => {
  assert.equal(await archivedQualification(root, relative, bytes), true);
}));
test("validates both historical inventory schemas and rejects conflicting digests", () => fixture(async ({ root, bytes, index, indexPath }) => {
  const digest = index.files["qualification-before.md"];
  for (const variant of [{ files: { "qualification-before.md": { sha256: digest } } }, { historicalDocumentSHA256: digest }]) {
    await writeFile(indexPath, JSON.stringify(variant));
    assert.equal(await archivedQualification(root, relative, bytes), true);
  }
  index.historicalDocumentSHA256 = "0".repeat(64);
  await writeFile(indexPath, JSON.stringify(index));
  await assert.rejects(archivedQualification(root, relative, bytes), /archived bytes changed/);
}));
test("rejects changed bytes and line-ending normalization", () => fixture(async ({ root, bytes }) => {
  await assert.rejects(archivedQualification(root, relative, Buffer.concat([bytes, Buffer.from("changed") ])), /archived bytes changed/);
  await assert.rejects(archivedQualification(root, relative, Buffer.from(bytes.toString().replaceAll("\r\n", "\n"))), /archived bytes changed/);
}));
test("requires an existing, well-formed adjacent inventory", () => fixture(async ({ root, bytes, indexPath }) => {
  await writeFile(indexPath, "invalid JSON");
  await assert.rejects(archivedQualification(root, relative, bytes), SyntaxError);
  await rm(indexPath);
  await assert.rejects(archivedQualification(root, relative, bytes), { code: "ENOENT" });
}));
test("rejects missing and invalid digest entries", () => fixture(async ({ root, bytes, index, indexPath }) => {
  for (const digest of [undefined, "", "f".repeat(63), "G".repeat(64), "0".repeat(64)]) {
    index.files["qualification-before.md"] = digest;
    await writeFile(indexPath, JSON.stringify(index));
    await assert.rejects(archivedQualification(root, relative, bytes));
  }
}));
test("keeps other documents under ordinary link and reachability validation", async () => {
  for (const other of ["docs/qualification-before.md", "docs/evidence/v0.3.0/2026-11-12-control/current.md", "docs/evidence/v0.3.0/undated/qualification-before.md", "docs/evidence/v0.3.0/2026-11-12-control/nested/qualification-before.md", "docs/evidence/v0.3.0/2026-11-12-control/qualification-before.md.extra", "docs/evidence/v0.3.0/../qualification-before.md"])
    assert.equal(await archivedQualification("missing-root", other, Buffer.from("untrusted")), false);
});

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import path from "node:path";

// These are byte-preserved copies of the qualification document before a
// dated evidence update. Their links retain the original document location.
// Current-document reachability and wording do not apply to old snapshots;
// their exact bytes must instead match their adjacent evidence inventory.
export async function archivedQualification(root, relativePath, bytes) {
  if (!/^docs\/evidence\/v0\.3\.0\/\d{4}-\d{2}-\d{2}-[a-z0-9-]+\/qualification-before\.md$/.test(relativePath)) return false;
  const index = JSON.parse(await readFile(path.join(root, path.dirname(relativePath), "index.json"), "utf8"));
  const entry = index.files?.["qualification-before.md"];
  const candidates = [typeof entry === "string" ? entry : entry?.sha256, index.historicalDocumentSHA256]
    .filter((value) => value !== undefined);
  assert.ok(candidates.length, `${relativePath} has no archived SHA256`);
  const actual = createHash("sha256").update(bytes).digest("hex");
  for (const expected of candidates) {
    assert.match(expected, /^[0-9a-f]{64}$/, `${relativePath} has no archived SHA256`);
    assert.equal(actual, expected, `${relativePath} archived bytes changed`);
  }
  return true;
}

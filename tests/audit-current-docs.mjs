import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import path from "node:path";

const root = path.resolve(process.argv[2] ?? ".");
const documentPaths = [
  "README.md",
  "bench/README.md",
  "bench/results/README.md",
  "bench/results/vue-ssr-sweep-r3-20260731/report.md",
  "docs/README.md",
  "docs/architecture.md",
  "docs/capability-policy.md",
  "docs/conformance-deviations.md",
  "docs/conformance-sources.md",
  "docs/embedding-api.md",
  "docs/escape-capabilities.md",
  "docs/host-architecture-plan.md",
  "docs/host-integration.md",
  "docs/host-technical-design-review.md",
  "docs/linux-sandbox.md",
  "docs/module-permissions.md",
  "docs/performance-benchmarks.md",
  "docs/project-status.md",
  "docs/standards-matrix.md",
  "docs/testing.md",
  "docs/txiki-upgrade-report.md",
  "docs/framework-compatibility/README.md",
  "docs/framework-compatibility/h3-v2.md",
  "docs/framework-compatibility/hono.md",
  "docs/framework-compatibility/itty-router.md",
];

const documents = new Map();
for (const relativePath of documentPaths) {
  documents.set(
    relativePath,
    await readFile(path.join(root, relativePath), "utf8"),
  );
}

for (const [relativePath, content] of documents) {
  for (const match of content.matchAll(/\[[^\]]+\]\(([^)]+)\)/g)) {
    let target = match[1].trim();
    if (
      target.startsWith("http://") ||
      target.startsWith("https://") ||
      target.startsWith("mailto:") ||
      target.startsWith("#")
    ) {
      continue;
    }
    if (target.startsWith("<") && target.endsWith(">")) {
      target = target.slice(1, -1);
    }
    target = decodeURIComponent(target.split("#", 1)[0]);
    if (!target) {
      continue;
    }
    const resolved = path.resolve(root, path.dirname(relativePath), target);
    await assert.doesNotReject(
      access(resolved),
      `${relativePath} has a broken local link: ${match[1]}`,
    );
  }
}

const capabilityManifest = JSON.parse(
  await readFile(path.join(root, "docs/capability-manifest.json"), "utf8"),
);
const modulePermissions = documents.get("docs/module-permissions.md");
for (const moduleName of capabilityManifest.modules.built_and_available) {
  assert.ok(
    documents.get("README.md").includes(`\`${moduleName}\``),
    `README.md does not list current module ${moduleName}`,
  );
  assert.ok(
    documents.get("docs/capability-policy.md").includes(`\`${moduleName}\``),
    `capability-policy.md does not explain current module ${moduleName}`,
  );
  assert.ok(
    modulePermissions.includes(`\`${moduleName}\``),
    `module-permissions.md does not explain current module ${moduleName}`,
  );
}
for (const permissionName of Object.keys(capabilityManifest.permissions)) {
  assert.ok(
    modulePermissions.includes(`\`${permissionName}\``),
    `module-permissions.md does not explain permission ${permissionName}`,
  );
}

const report = JSON.parse(
  await readFile(path.join(root, "docs/txiki-upgrade-report.json"), "utf8"),
);
const baselineText =
  `${report.tests.registered} 项：\n` +
  `普通宿主实测 ${report.tests.passed} 通过、` +
  `${report.tests.skipped.length} 个环境型 skip、0 失败`;
assert.ok(
  documents.get("docs/testing.md").includes(baselineText),
  "docs/testing.md does not match the generated test report",
);

const projectStatus = documents.get("docs/project-status.md");
const activeSection = projectStatus
  .split("## 活跃事项", 2)[1]
  ?.split("## ", 1)[0] ?? "";
assert.deepEqual(
  [...activeSection.matchAll(/TODO-P\d-\d+/g)].map((match) => match[0]),
  ["TODO-P2-04"],
  "project-status.md active section must contain only the actual open item",
);

const readme = documents.get("README.md");
for (const requiredFragment of [
  "cmake --install build-release",
  "<capsid/runtime.hpp>",
  "`egress_policy == NULL` 时出站 Fetch 全部拒绝",
  "`strict_sandbox` 默认是关闭的",
  "`tjs:*` 不能通过配置开放",
  "`CAPSID_EVENT_READY.flags` 必须包含部署要求的 sandbox feature",
]) {
  assert.ok(
    readme.includes(requiredFragment),
    `README.md is missing user-facing configuration guidance: ${requiredFragment}`,
  );
}

const currentDocs = [...documents.values()].join("\n");
for (const staleFragment of [
  "vue-ssr-sweep-r1-20260730",
  "app-v2.ts",
  "app-v3.ts",
  'add_argument("--rounds"',
  "--rounds 3",
  "Capsid Runtime",
  "其余普通扩展仍 unavailable",
]) {
  assert.ok(
    !currentDocs.includes(staleFragment),
    `current documentation contains stale fragment: ${staleFragment}`,
  );
}

console.log(
  `current docs: ${documentPaths.length} documents, ` +
    `${capabilityManifest.modules.built_and_available.length} modules, ` +
    `${report.tests.registered} tests verified`,
);

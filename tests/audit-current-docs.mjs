import assert from "node:assert/strict";
import { access, readFile, readdir } from "node:fs/promises";
import path from "node:path";

const root = path.resolve(process.argv[2] ?? ".");

async function markdownFiles(relativeRoot) {
  const absoluteRoot = path.join(root, relativeRoot);
  const entries = await readdir(absoluteRoot, { withFileTypes: true });
  const result = [];
  for (const entry of entries) {
    if (entry.name === "node_modules") {
      continue;
    }
    const relativePath = path.join(relativeRoot, entry.name);
    if (entry.isDirectory()) {
      result.push(...await markdownFiles(relativePath));
    } else if (entry.isFile() && entry.name.endsWith(".md")) {
      result.push(relativePath);
    }
  }
  return result;
}

const documentPaths = [
  "README.md",
  "CONTRIBUTING.md",
  "SECURITY.md",
  ...await markdownFiles("docs"),
  ...await markdownFiles("examples"),
].map((value) => value.split(path.sep).join("/")).sort();

const documents = new Map();
for (const relativePath of documentPaths) {
  documents.set(
    relativePath,
    // Normalize CRLF checkouts (git autocrlf on Windows) so the frozen
    // fragment matching below is line-ending agnostic.
    (await readFile(path.join(root, relativePath), "utf8")).replace(
      /\r\n/g,
      "\n",
    ),
  );
}

function localLinks(relativePath, content) {
  const links = [];
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
    links.push({
      original: match[1],
      resolved: path.resolve(root, path.dirname(relativePath), target),
    });
  }
  return links;
}

for (const [relativePath, content] of documents) {
  for (const link of localLinks(relativePath, content)) {
    await assert.doesNotReject(
      access(link.resolved),
      `${relativePath} has a broken local link: ${link.original}`,
    );
  }
}

// Every maintained docs/*.md file must be reachable from the docs index. This
// permits a small hierarchy (for example the framework index) without allowing
// an unindexed orphan document to accumulate.
const docsPaths = documentPaths.filter((value) => value.startsWith("docs/"));
const reachable = new Set(["docs/README.md"]);
const pending = ["docs/README.md"];
while (pending.length > 0) {
  const source = pending.pop();
  for (const link of localLinks(source, documents.get(source))) {
    const relativeTarget = path.relative(root, link.resolved)
      .split(path.sep).join("/");
    if (
      relativeTarget.startsWith("docs/") &&
      relativeTarget.endsWith(".md") &&
      documents.has(relativeTarget) &&
      !reachable.has(relativeTarget)
    ) {
      reachable.add(relativeTarget);
      pending.push(relativeTarget);
    }
  }
}
assert.deepEqual(
  docsPaths.filter((value) => !reachable.has(value)),
  [],
  "docs/README.md does not reach every maintained Markdown document",
);

const capabilityManifest = JSON.parse(
  await readFile(path.join(root, "docs/capability-manifest.json"), "utf8"),
);
const readme = documents.get("README.md");
const architecture = documents.get("docs/architecture.md");
const hostDesign = documents.get("docs/host-technical-design-review.md");
const performanceGuide = documents.get("docs/performance-benchmarks.md");
const testingGuide = documents.get("docs/testing.md");
const capabilityPolicy = documents.get("docs/capability-policy.md");
const modulePermissions = documents.get("docs/module-permissions.md");
for (const moduleName of capabilityManifest.modules.built_and_available) {
  assert.ok(
    readme.includes(`\`${moduleName}\``),
    `README.md does not list current module ${moduleName}`,
  );
  assert.ok(
    capabilityPolicy.includes(`\`${moduleName}\``),
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

for (const [documentName, content, requiredFragments] of [
  [
    "docs/architecture.md",
    architecture,
    [
      "平台支持分为“原生开发”和“生产隔离”两个独立承诺",
      "Windows 原生开发链路自 v0.1.2 起交付",
      "Host 决定所需能力并在 READY 时验证实际 feature",
      "Runtime 实现进程创建、IPC、终止/回收和 OS sandbox",
    ],
  ],
  [
    "docs/host-technical-design-review.md",
    hostDesign,
    [
      "不进入 M1 发布门；M1A",
      "M1A + M1B 作为同一实施批次交付",
      "M1A：benchmark-minimal 单 worker 数据面",
      "绿后立即运行 15.7 的首轮 baseline",
      "TSan 可以晚于该首轮 baseline",
      "必须早于 M1C 验收和 M2 多 worker 实施",
      "M1C/M1D",
      "直接调用 `capsid_worker_fd()`",
      "Windows 机器/hosted runner 时不实现",
      "seccomp/Landlock bit 表达不同保证",
    ],
  ],
  [
    "docs/testing.md",
    documents.get("docs/testing.md"),
    [
      "TSan（M1C 门）",
      "M1C 验收前必须通过",
      "不与 ASan、UBSan、LTO、fuzz 或 benchmark\n混跑",
      "任何第一方代码报告均失败",
      "personality(ADDR_NO_RANDOMIZE)",
      "seccomp=unconfined",
    ],
  ],
  [
    "docs/performance-benchmarks.md",
    performanceGuide,
    [
      "首轮不等待 request\nbody、streaming、cancel 或 timeout 实现",
      "这些契约完成后必须用同一 runner",
    ],
  ],
  [
    "docs/testing.md",
    testingGuide,
    [
      "Windows native-dev 使用 `windows-latest` hosted runner",
      "Windows 交叉编译、Wine、WSL2 或 Linux 容器不能替代 hosted Windows 原生运行证据",
      "五类 job 都产出 hosted evidence",
    ],
  ],
]) {
  for (const fragment of requiredFragments) {
    assert.ok(
      content.includes(fragment),
      `${documentName} is missing the frozen platform contract: ${fragment}`,
    );
  }
}

const removedDocuments = [
  "docs/host-architecture-plan.md",
  "docs/project-status.md",
  "docs/txiki-upgrade-report.md",
  "docs/txiki-upgrade-report.json",
];
for (const relativePath of removedDocuments) {
  await assert.rejects(
    access(path.join(root, relativePath)),
    `${relativePath} is a removed snapshot or superseded document`,
  );
}

const currentDocs = [...documents.values()].join("\n");
for (const staleFragment of [
  "host-architecture-plan.md",
  "project-status.md",
  "txiki-upgrade-report.md",
  "bench/README.md",
  "](../bench/results/",
  "vue-ssr-sweep-r1-20260730",
  "app-v2.ts",
  "app-v3.ts",
  'add_argument("--rounds"',
  "--rounds 3",
  "其余普通扩展仍 unavailable",
]) {
  assert.ok(
    !currentDocs.includes(staleFragment),
    `current documentation contains stale fragment: ${staleFragment}`,
  );
}

console.log(
  `current docs: ${documentPaths.length} documents, ` +
    `${capabilityManifest.modules.built_and_available.length} modules verified`,
);

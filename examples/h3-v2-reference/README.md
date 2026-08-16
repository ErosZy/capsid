# H3 v2 reference app

This directory pins the H3 version used by the Capsid Runtime compatibility suite. Install dependencies and generate the self-contained ESM:

```sh
npm ci --ignore-scripts
npm run build
```

The reference controller and the real worker bundle share the same application logic. Build artifacts are for testing only; CMake also audits external/dynamic imports, Node/server adapters, platform globals, and size boundaries.

See the verification scope, exclusions, and test commands in
[`../../docs/framework-compatibility/h3-v2.md`](../../docs/framework-compatibility/h3-v2.md).

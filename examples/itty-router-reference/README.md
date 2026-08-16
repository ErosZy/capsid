# itty-router reference app

This directory pins the itty-router version used by the Capsid Runtime compatibility suite:

```sh
npm ci --ignore-scripts
npm run build
```

`src/shared-handlers.js` is used by both the Node reference controller and the real worker bundle. The three entries cover:

- default `AutoRouter`;
- `Router`'s default `{ fetch: router.fetch }`;
- the named `fetch` for the hand-written `IttyRouter` promise pipeline.

CMake uses the same `build.mjs` and audits each self-contained ESM before worker tests load it. See the verification scope, exclusions, and differential rules in
[`../../docs/framework-compatibility/itty-router.md`](../../docs/framework-compatibility/itty-router.md).

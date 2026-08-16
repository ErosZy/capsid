# Hono Reference App

This directory pins the Hono version used by the Capsid Runtime compatibility suite:

```sh
npm ci --ignore-scripts
```

`src/app.js` serves as both the Node `app.request()` reference and the real worker bundle.
The three entry points cover the allowed export forms. CMake/esbuild generates a self-contained ESM for each entry point;
the app runtime never imports from this directory.

For the verification scope, exclusions, and test commands, see
[`../../docs/framework-compatibility/hono.md`](../../docs/framework-compatibility/hono.md).

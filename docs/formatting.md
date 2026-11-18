# Source Formatting

Use the repository's pinned tools and one entry point. Formatting is separate
from lint fixes, runtime behavior changes, release qualification and benchmarks.

## Tools and Style

| Source | Tool | Policy |
| --- | --- | --- |
| C/C++ | clang-format 23.1.5 | LLVM-derived, 4 spaces, 100 columns, left-aligned pointers; no include sorting, string splitting or comment reflow |
| JavaScript | Prettier 3.8.3 | 2 spaces, 100 columns, single quotes, preserved property quoting, LF; no embedded-language formatting |
| Selected development JSON | Prettier 3.8.3 | 2 spaces; parsed values must remain equal |
| Python | Ruff 0.17.5 | Python 3.12, 4 spaces, 100 columns, double quotes, LF; no import-sorting or lint autofix |

Acorn 8.15.0 independently compares JavaScript ASTs before writing, including
BigInt values and tagged-template raw strings. Prettier's CLI debug checker
cannot serialize some BigInt AST values, so the wrapper uses this explicit
comparison instead of skipping semantic checks. Regex flag order is canonical;
changes to flags, patterns, operators or values still fail.

`.editorconfig` supplies editor defaults and `.gitattributes` fixes maintained-code LF
checkout behavior on Windows. JSON normalization is limited to selected development
files; generated source locks retain their exact bytes. Blanket JSON/YAML EOL rules
can make a freshly checked-out repository dirty and correctly block Release builds. Formatter versions are exact: Python tools in
`tools/format-requirements.txt`, JavaScript tools and integrity in
`package.json` / `package-lock.json`.

## Setup

Use Node 24 and Python 3.12+. Dependencies are development-only, not linked into
the runtime. Do not install the Python tools globally.

Linux/macOS:

```sh
python3 -m venv .venv-format
.venv-format/bin/python -m pip install --only-binary=:all: -r tools/format-requirements.txt
npm ci --ignore-scripts
npm run format:check
```

Windows PowerShell:

```powershell
python -m venv .venv-format
if ($LASTEXITCODE -ne 0) { throw 'venv creation failed' }
& ./.venv-format/Scripts/python.exe -m pip install --only-binary=:all: -r tools/format-requirements.txt
if ($LASTEXITCODE -ne 0) { throw 'formatter installation failed' }
npm.cmd ci --ignore-scripts
if ($LASTEXITCODE -ne 0) { throw 'npm install failed' }
npm.cmd run format:check
if ($LASTEXITCODE -ne 0) { throw 'format check failed' }
```

The runner finds `.venv-format/bin` or `.venv-format/Scripts` without activation.
It rejects missing/wrong formatter versions. `CAPSID_FORMAT_PYTHON` can select
an explicit Python interpreter; `--tool-dir` selects an alternate installed
Python-tool directory without changing PATH or global configuration.

## Commands

```sh
npm run format                 # apply all maintained-source formatting
npm run format:check           # read-only, nonzero for any mismatch
npm run format:list            # exact selected files, no tools needed
python3 tools/format.py --check --language cpp
python3 tools/format.py --write src/host/config.cc tests/test_host_admin_http.cc
```

On Windows use `npm.cmd` and `python`. Paths are repository-relative. The
runner batches argv safely on Windows and never traverses dependency/build
trees. An empty selection, unsupported/protected explicit file or version
mismatch is an error, not a silent successful check.

## Preserved Inputs

`tools/format-policy.json` is the shared selection allowlist. The default
covers maintained C/C++, JS and Python under src, include, js, tests, tools,
bench, soak, examples and scripts, plus selected development package/config JSON.

Never apply a blanket formatter to:

- vendor, patches, licenses and installed dependencies;
- `tests/fixtures`, the frozen ABI compatibility header and pinned `tests/wpt` inputs;
- the explicit first-party source inputs in `preservedInputs` (source-hash-bound
  primary capture models/clients/drivers and the mutation runner template);
- generated `js/streams.js`, `js/encoding-data` tables and `tools/encoding-data` locks;
- benchmark result directories, performance data/transports or captured reports;
- historical evidence/snapshots or machine-generated documentation status.

These are exact-byte inputs or are maintained through their own generators.
Their omission is deliberate, not permission to normalize them manually.
`preservedInputs` records exact SHA256 identities and the captures/consumers that
bind them; CI verifies these bytes as well as refusing their selection. JS AST
equivalence alone cannot preserve raw source hashes or textual mutation templates.
Markdown, CMake, shell and existing workflows keep their established styles;
this command does not claim to provide formatters for every file language.

## CI and Evidence

The independent **Formatting** workflow checks the same selection and pinned
versions, then exercises selection guards and AST corruption controls. It does
not replace the existing runtime/test workflows or change their budgets.

Source formatting and five-platform native packaging are included in the published
`0.3.0-beta` (`v0.3.0-beta.3`) release (`af8acc1`). For subsequent commits,
`docs/current-status.json` keeps qualified-release digests bound to the declared
release commit and explicitly denies inherited qualification (`releaseQualificationInherited:false`).
Later changes are never classified as qualified binary or performance evidence
without their own matching run. Neither unchanged tokens/ASTs nor a green
format check requalify a new binary, trusted bytecode identity, sandbox
deployment or performance result. See [current status](current-status.md) and
[testing](testing-guide.md).

# Getting Started

Run one **trusted local** Fetch handler using the first-party Host. This tutorial
uses `single-worker`, loopback networking and `--strict-sandbox off` so it can
be used on Linux, macOS and Windows. It is not an isolation or production
qualification procedure.

For untrusted applications, first read the [security boundary](../SECURITY.md)
and [Linux strict sandbox contract](linux-sandbox.md). macOS and Windows do not
provide that production isolation boundary.

## 1. Obtain the Tools

These commands target the current source interface and the **v0.3.0-beta.2**
package. Choose that channel explicitly in [releases and installation](releases.md),
or follow [building from source](building.md). The default/latest installer still
selects stable v0.2.2; this tutorial does not requalify that version.

Keep `capsid-host` and `capsid-worker` from the same build together. A runtime
version string alone does not establish build or bytecode compatibility.

The commands below use the package's `bin` directory. If you built from source,
use the build directory containing the executables instead.

## 2. Create the Application

Create a working directory owned by the account that will run the Host, then
save this as UTF-8 `app.bundle.js`. Configuration ownership/permission checks
still apply with `--strict-sandbox off`. In WSL, use your Linux home or another
private native-Linux directory: a Windows-mounted file can have a different
reported owner. Do not disable the checks or change unrelated files' ownership.

```js
export default {
  fetch(request) {
    return Response.json({
      message: "hello from Capsid",
      path: new URL(request.url).pathname,
    });
  },
};
```

This is already a self-contained ESM: it needs no package installation or
bundling step. Applications with dependencies must be bundled ahead of time;
see the [framework guides](framework-compatibility/README.md).

Save this as UTF-8 `capsid.json`. It requests no capability modules or outbound
Fetch targets:

```json
{
  "apiVersion": "capsid/app-v1",
  "permissions": {
    "modules": [],
    "fetch": { "allow": [] }
  },
  "pool": { "minReady": 1, "maxWorkers": 1 }
}
```

Do not put credentials in either file. The [configuration guide](capsid-json.md)
explains how to request additional authority explicitly.

## 3. Start the Host

Use a free loopback port. If startup fails, read stderr and correct the cause;
there is no automatic port selection or business-request retry in this example.

### Linux and macOS

After installing to the default prefix:

```sh
CAPSID_BIN="$HOME/.local/bin"

"$CAPSID_BIN/capsid-host" --mode single-worker \
  --worker "$CAPSID_BIN/capsid-worker" \
  --source-bundle ./app.bundle.js \
  --source-name "file://$PWD/app.bundle.js" \
  --application hello --listen 127.0.0.1:8080 \
  --routing path --public-scheme http \
  --public-authority 127.0.0.1:8080 \
  --request-timeout-ms 5000 --strict-sandbox off \
  --ready-fd 1 --capsid-json ./capsid.json
```

Set `CAPSID_BIN` to the actual `bin` directory if you extracted the archive
manually, or to your source build directory.

### Windows PowerShell

Point `$capsidBin` at the actual extracted package's `bin` directory:

```powershell
$capsidBin = 'C:/tools/capsid/bin'
$bundle = (Resolve-Path -LiteralPath './app.bundle.js').Path
$sourceName = ([System.Uri]$bundle).AbsoluteUri

& "$capsidBin/capsid-host.exe" --mode single-worker `
  --worker "$capsidBin/capsid-worker.exe" `
  --source-bundle $bundle --source-name $sourceName `
  --application hello --listen 127.0.0.1:8080 `
  --routing path --public-scheme http `
  --public-authority 127.0.0.1:8080 `
  --request-timeout-ms 5000 --strict-sandbox off `
  --ready-fd 1 --capsid-json ./capsid.json
```

### Wait for Readiness

`--ready-fd 1` explicitly directs the one-line readiness record to stdout for
this interactive example. Diagnostics go to stderr. Successful startup produces:

```json
{"schema":"capsid-host-ready-v1","app":"hello","address":"127.0.0.1","port":8080}
```

Wait for this record before sending the request. File existence, process
creation or an empty log is not readiness. Supervising Hosts should use a
separate inherited readiness descriptor and, when needed, the `--stop-fd`
protocol; see [Host configuration](host-config.md).

## 4. Send a Request

Leave the Host running. In another terminal:

```sh
curl http://127.0.0.1:8080/@capsid/hello/
```

In Windows PowerShell, use `curl.exe` rather than the `curl` alias:

```powershell
curl.exe http://127.0.0.1:8080/@capsid/hello/
```

Expected JSON:

```json
{"message":"hello from Capsid","path":"/"}
```

The `path` routing mode removes `/@capsid/hello` before constructing the
application request URL. Stop the foreground Host when finished, normally with
Ctrl+C. This example does not install a system service or expose a public port.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `missing --...` | Use the complete command. Public authority, strict-sandbox mode and a valid readiness descriptor are explicit CLI inputs. |
| `--ready-fd is not an open descriptor` | The chosen descriptor must be inherited and open. The interactive example uses stdout, descriptor 1. |
| Listener bind failure | Confirm the address is loopback and the port is free; stop only processes you own. |
| Import or outbound Fetch denied | Add the narrow request to `capsid.json`; do not broaden policy or disable security checks to hide a failure. |
| Configuration is not owned by the current user | Create the example files under the same account on a suitable filesystem. In WSL, prefer a private native-Linux directory rather than a Windows-mounted workspace; do not bypass the ownership check. |
| Strict sandbox unavailable | Use the Linux prerequisites for untrusted workloads. Turning isolation off is only appropriate for trusted development. |
| Bytecode compatibility rejection | Use artifacts and compiler from the matching build; do not bypass identity checks. |

## Next Steps

- [Application configuration](capsid-json.md): modules, egress, resources and
  entry points.
- [Host modes and options](host-config.md): static pools and Linux managed mode.
- [Host integration](host-integration.md): embedding the C/C++ runtime.
- [Testing guide](testing-guide.md): what a local smoke test does—and does not—prove.

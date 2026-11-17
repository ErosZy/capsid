import assert from 'node:assert/strict';
import { test } from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import http from 'node:http';
import { execFileSync } from 'node:child_process';
import { startCapsid, stopCapsid } from './helpers/ws_host_backend.mjs';

// This host exercises the actual startup transport and process cleanup. It
// supplies no product or workerd conformance evidence.
const fakeHost = `#!${process.execPath}
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
const config=JSON.parse(fs.readFileSync(process.argv[process.argv.indexOf('--host-config')+1]));
const fault=process.env.CAPSID_TEST_ADMIN_FAULT;
const server=http.createServer((request,response)=>{
    request.resume();
    request.on('end',()=>{
        if((fault==='deploy-reset'&&request.url==='/v1/deploy')||
            (fault==='operation-reset'&&request.url.startsWith('/v1/operations/'))){
            request.socket.destroy();return;
        }
        if(fault==='invalid-json'&&request.url==='/v1/deploy'){
            response.writeHead(202,{'Connection':'close'});response.end('invalid JSON');return;
        }
        if(fault==='truncated-response'&&request.url.startsWith('/v1/operations/')){
            response.writeHead(200,{'Content-Length':'1000','Connection':'close'});
            response.flushHeaders();response.write('{');
            setImmediate(()=>response.destroy());return;
        }
        response.writeHead(request.url==='/v1/deploy'?202:200,{'Connection':'close'});
        response.end(JSON.stringify(request.url==='/v1/deploy'?{operationId:'operation-1'}:{state:'active'}));
    });
});
const data=net.createServer();
process.on('SIGTERM',()=>{
    process.stderr.write('diagnostic host stopped\\n');
    server.close(()=>data.close(()=>process.exit(0)));
});
data.listen({host:'127.0.0.1',port:Number(config.listeners[0].tcp.split(':').at(-1)),exclusive:true},()=>{
    fs.writeSync(3,JSON.stringify({schema:'capsid-host-ready-v1',port:data.address().port})+'\\n');
    server.listen(config.admin.unix,()=>process.stderr.write('diagnostic host ready\\n'));
});
`;

async function fixture(operation) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-startup-'));
  try {
    const host = path.join(directory, 'fake-host.mjs'),
      bundle = path.join(directory, 'bundle.mjs');
    fs.writeFileSync(host, fakeHost, { mode: 0o700 });
    fs.writeFileSync(bundle, 'export default {};');
    await operation({ directory, host, bundle });
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
}

for (const [fault, method, target] of [
  ['deploy-reset', 'POST', '/v1/deploy'],
  ['operation-reset', 'GET', '/v1/operations/operation-1'],
  ['invalid-json', 'POST', '/v1/deploy'],
  ['truncated-response', 'GET', '/v1/operations/operation-1'],
]) {
  test(
    `managed startup retains ${fault} request, stderr and natural cleanup`,
    { skip: process.platform === 'win32', timeout: 10000 },
    async () =>
      fixture(async ({ directory, host, bundle }) => {
        await assert.rejects(
          startCapsid(host, 'unused-worker', bundle, directory, 'managed', {
            env: { CAPSID_TEST_ADMIN_FAULT: fault },
          }),
          (error) => {
            assert.ok(error.cause instanceof Error);
            assert.ok(error.message.includes(`admin ${method} ${target}`));
            if (fault.endsWith('reset')) assert.equal(error.code, 'ECONNRESET');
            const observation = error.observation;
            assert.equal(observation.phase, 'capsid-startup');
            assert.deepEqual(observation.adminRequest, { method, target });
            assert.ok(observation.failure.stack.includes(`admin ${method} ${target}`));
            assert.equal(typeof observation.failure.causeStack, 'string');
            if (fault.endsWith('reset')) assert.equal(observation.failure.code, 'ECONNRESET');
            assert.match(observation.stderr, /diagnostic host ready/);
            assert.match(observation.stderr, /diagnostic host stopped/);
            assert.deepEqual(observation.shutdown, { code: 0, signal: null, forced: false });
            assert.equal(observation.cleanupFailure, null);
            assert.deepEqual(JSON.parse(JSON.stringify(observation)), observation);
            assert.throws(() => process.kill(observation.pid, 0), { code: 'ESRCH' });
            return true;
          },
        );
      }),
  );
}

test(
  'healthy managed startup still deploys and stops normally',
  { skip: process.platform === 'win32', timeout: 10000 },
  async () =>
    fixture(async ({ directory, host, bundle }) => {
      const foreign = net.createServer();
      await new Promise((resolve, reject) => {
        foreign.once('error', reject);
        foreign.listen(0, '127.0.0.1', resolve);
      });
      let backend;
      try {
        backend = await startCapsid(host, 'unused-worker', bundle, directory, 'managed', {
          env: { CAPSID_TEST_ADMIN_FAULT: 'healthy' },
        });
        assert.equal(
          JSON.parse(fs.readFileSync(path.join(directory, 'host.json'), 'utf8')).listeners[0].tcp,
          '127.0.0.1:0',
        );
        assert.ok(backend.port > 0);
        assert.notEqual(
          backend.port,
          foreign.address().port,
          'not an unrelated namespace listener',
        );
        const competitor = net.createServer();
        await assert.rejects(
          new Promise((resolve, reject) => {
            competitor.once('error', reject);
            competitor.listen(backend.port, '127.0.0.1', resolve);
          }),
          { code: 'EADDRINUSE' },
        );
        assert.equal((await backend.admin('/v1/operations/operation-1')).json.state, 'active');
      } finally {
        if (backend)
          assert.deepEqual(await stopCapsid(backend.child), {
            code: 0,
            signal: null,
            forced: false,
          });
        await new Promise((resolve) => foreign.close(resolve));
      }
    }),
);

// A real AF_UNIX socket can exist after bind() but before listen(). Keep
// that kernel state until the controller has actually observed refusal;
// there is no sleep-sized race window or successful deployment retry here.
function writeBoundAdminHost(host) {
  const python =
    process.env.CAPSID_WS_INTEROP_PYTHON ??
    execFileSync('python3', ['-c', 'import sys;print(sys.executable)'], {
      encoding: 'utf8',
    }).trim();
  assert.ok(path.isAbsolute(python));
  fs.writeFileSync(
    host,
    `#!${python}
import http.server,json,os,signal,socket,socketserver,sys,time
from pathlib import Path
config=json.loads(Path(sys.argv[sys.argv.index('--host-config')+1]).read_text())
address=config['admin']['unix'];folder=Path(address).parent
data=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
data.bind(('127.0.0.1',int(config['listeners'][0]['tcp'].rsplit(':',1)[1])))
data.listen()
os.write(3,(json.dumps({'schema':'capsid-host-ready-v1','port':data.getsockname()[1]})+'\\n').encode())
refused=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
refused.bind(str(folder/'refuse.sock'))
requests=[]
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args): pass
    def do_POST(self): self.reply()
    def do_GET(self): self.reply()
    def reply(self):
        self.rfile.read(int(self.headers.get('Content-Length','0')))
        requests.append({'method':self.command,'target':self.path})
        (folder/'requests.json').write_text(json.dumps(requests))
        body=json.dumps({'operationId':'operation-1'} if self.command=='POST' else {'state':'active'}).encode()
        self.send_response(202 if self.command=='POST' else 200)
        self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(body)))
        self.send_header('Connection','close')
        self.end_headers();self.wfile.write(body)
server=socketserver.UnixStreamServer(address,Handler,bind_and_activate=False)
def stop(*args):
    server.server_close();refused.close();data.close()
    print('diagnostic host stopped',file=sys.stderr,flush=True)
    sys.exit(0)
signal.signal(signal.SIGTERM,stop)
server.server_bind()
print('diagnostic host bound',file=sys.stderr,flush=True)
if os.environ.get('CAPSID_TEST_ADMIN_FAULT')=='bound-before-listen':
    while not (folder/'allow-listen').exists(): time.sleep(0.001)
server.server_activate()
print('diagnostic host ready',file=sys.stderr,flush=True)
server.serve_forever(poll_interval=0.01)
`,
    { mode: 0o700 },
  );
}

test(
  'managed startup waits for actual listen after the socket has been bound',
  { skip: process.platform === 'win32', timeout: 10000 },
  async () =>
    fixture(async ({ directory, host, bundle }) => {
      writeBoundAdminHost(host);
      const socketPath = path.join(directory, 'run', 'admin.sock');
      const originalConnect = net.createConnection,
        originalRequest = http.request;
      let refusals = 0,
        posts = 0,
        backend;
      net.createConnection = function (options, ...rest) {
        const socket = originalConnect.call(net, options, ...rest);
        if (options.path === socketPath)
          socket.once('error', (error) => {
            if (error.code === 'ECONNREFUSED') {
              ++refusals;
              fs.writeFileSync(path.join(directory, 'run', 'allow-listen'), 'ready');
            }
          });
        return socket;
      };
      http.request = function (options, ...rest) {
        if (options.socketPath === socketPath && options.method === 'POST') ++posts;
        return originalRequest.call(http, options, ...rest);
      };
      try {
        backend = await startCapsid(host, 'unused-worker', bundle, directory, 'managed', {
          env: { CAPSID_TEST_ADMIN_FAULT: 'bound-before-listen' },
        });
        assert.ok(refusals > 0, 'the real pre-listen refusal must have occurred');
        assert.equal(posts, 1, 'readiness must precede the one deployment POST');
        assert.deepEqual(
          JSON.parse(fs.readFileSync(path.join(directory, 'run', 'requests.json'), 'utf8')),
          [
            { method: 'POST', target: '/v1/deploy' },
            { method: 'GET', target: '/v1/operations/operation-1' },
          ],
        );
      } finally {
        net.createConnection = originalConnect;
        http.request = originalRequest;
        if (backend)
          assert.deepEqual(await stopCapsid(backend.child), {
            code: 0,
            signal: null,
            forced: false,
          });
      }
    }),
);

test(
  'a refused deployment POST remains a failure and is never retried',
  { skip: process.platform === 'win32', timeout: 10000 },
  async () =>
    fixture(async ({ directory, host, bundle }) => {
      writeBoundAdminHost(host);
      const socketPath = path.join(directory, 'run', 'admin.sock'),
        originalRequest = http.request;
      let posts = 0,
        backend;
      http.request = function (options, ...rest) {
        if (options.socketPath === socketPath && options.method === 'POST' && ++posts === 1)
          options = { ...options, socketPath: path.join(directory, 'run', 'refuse.sock') };
        return originalRequest.call(http, options, ...rest);
      };
      try {
        const attempt = startCapsid(host, 'unused-worker', bundle, directory, 'managed').then(
          (value) => {
            backend = value;
            return value;
          },
        );
        await assert.rejects(attempt, (error) => {
          assert.equal(error.code, 'ECONNREFUSED');
          assert.deepEqual(error.observation.adminRequest, {
            method: 'POST',
            target: '/v1/deploy',
          });
          assert.deepEqual(error.observation.shutdown, { code: 0, signal: null, forced: false });
          assert.match(error.observation.stderr, /diagnostic host stopped/);
          return true;
        });
        assert.equal(posts, 1);
      } finally {
        http.request = originalRequest;
        if (backend) await stopCapsid(backend.child);
      }
    }),
);

test(
  'invalid readiness retains the nonzero child exit after its stop channel closes',
  { skip: process.platform === 'win32', timeout: 10000 },
  async () =>
    fixture(async ({ directory, host, bundle }) => {
      fs.writeFileSync(
        host,
        `#!${process.execPath}
import fs from 'node:fs';
fs.closeSync(4);
setTimeout(()=>{
    fs.writeSync(3,'invalid readiness JSON\\n');
    setTimeout(()=>process.exit(7),50);
},50);
`,
        { mode: 0o700 },
      );
      await assert.rejects(startCapsid(host, 'unused-worker', bundle, directory), (error) => {
        assert.ok(error.cause instanceof SyntaxError);
        assert.equal(error.observation.mode, 'single-worker');
        assert.deepEqual(error.observation.shutdown, { code: 7, signal: null, forced: false });
        assert.equal(error.observation.cleanupFailure, null);
        assert.throws(() => process.kill(error.observation.pid, 0), { code: 'ESRCH' });
        return true;
      });
    }),
);

// Product managed mode and its /proc ownership path are Linux-only. Corrupt
// only observer metadata here, never an HTTP response or the child's sockets.
if (process.platform === 'linux')
  for (const fault of ['changed-start', 'foreign-inodes', 'ambiguous-listeners']) {
    test(
      `managed listener capture rejects ${fault} without guessing a port`,
      { timeout: 10000 },
      async () =>
        fixture(async ({ directory, host, bundle }) => {
          const originalRead = fs.readFileSync;
          let statReads = 0,
            corrupted = 0,
            backend;
          fs.readFileSync = function (file, ...args) {
            const value = originalRead.call(fs, file, ...args);
            if (typeof file !== 'string') return value;
            if (
              fault === 'changed-start' &&
              /^\/proc\/\d+\/stat$/.test(file) &&
              ++statReads === 2
            ) {
              ++corrupted;
              const end = value.lastIndexOf(')'),
                fields = value
                  .slice(end + 2)
                  .trim()
                  .split(/\s+/);
              fields[19] = String(BigInt(fields[19]) + 1n);
              return value.slice(0, end + 2) + fields.join(' ') + '\n';
            }
            if (fault !== 'changed-start' && /^\/proc\/\d+\/net\/tcp$/.test(file)) {
              ++corrupted;
              const lines = value.trim().split('\n'),
                rows = lines.slice(1).map((line) => line.trim().split(/\s+/));
              if (fault === 'foreign-inodes')
                return (
                  lines[0] +
                  '\n' +
                  rows
                    .map((row) => {
                      row[9] = '0';
                      return row.join(' ');
                    })
                    .join('\n') +
                  '\n'
                );
              const extra = rows
                .filter((row) => row[3] === '0A' && row[1].startsWith('0100007F:'))
                .map((row) => {
                  const copy = [...row];
                  copy[1] = '0100007F:' + (row[1].endsWith(':0001') ? '0002' : '0001');
                  return copy.join(' ');
                });
              assert.ok(extra.length > 0);
              return value + '\n' + extra.join('\n') + '\n';
            }
            return value;
          };
          try {
            await assert.rejects(
              startCapsid(host, 'unused-worker', bundle, directory, 'managed').then((value) => {
                backend = value;
                return value;
              }),
              (error) => {
                assert.match(
                  error.message,
                  fault === 'changed-start'
                    ? /same owned host process/
                    : /one owned IPv4 loopback data listener/,
                );
                assert.deepEqual(error.observation.shutdown, {
                  code: 0,
                  signal: null,
                  forced: false,
                });
                assert.equal(error.observation.cleanupFailure, null);
                return true;
              },
            );
            assert.equal(corrupted, 1, 'the intended metadata boundary was exercised');
          } finally {
            fs.readFileSync = originalRead;
            if (backend) await stopCapsid(backend.child);
          }
        }),
    );
  }

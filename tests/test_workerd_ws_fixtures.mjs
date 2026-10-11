// workerd WebSocket fixture runner (v0.3.0 WS-A stage).
//
// The fixtures under tests/fixtures/workerd-ws/ are the differential +
// absolute-assertion pack from docs/v0.3.0-fetchrpc-v4-plan.md (WS-A).
// They are worker scripts shaped to run on pinned workerd (differential)
// and, once the WebSocket vertical slice lands, on the capsid host.
//
// TDD gate: until `WebSocketPair` appears in the Capsid profile
// (js/profile-manifest.js), every fixture is SKIP (exit 77) with the
// reason below — the red state is visible and the flip condition is
// mechanical. When the profile gains the API, this runner grows the
// host spawn + real WebSocket client harness (WS-E2) and the fixtures
// must pass without modification.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const fixtureDir = path.join(here, 'fixtures', 'workerd-ws');

// The profile manifest is the single recorded source of the Capsid
// global surface (ROADMAP: additions and removals are observable
// profile changes).
const manifestPath = path.join(here, '..', 'js', 'profile-manifest.js');
const manifestText = fs.readFileSync(manifestPath, 'utf8');
const profileHasWebSocketPair = /'WebSocketPair'/.test(manifestText);

const fixtures = fs
    .readdirSync(fixtureDir)
    .filter((name) => name.endsWith('.mjs'))
    .sort();

assert.ok(fixtures.length >= 6, 'fixture pack carries its scenarios');

if (!profileHasWebSocketPair) {
    console.log(
        `SKIP: WebSocketPair is not in the Capsid profile yet ` +
        `(${fixtures.length} fixtures parked: ${fixtures.join(', ')})`);
    process.exit(77);
}

// WS-E2 harness lands with the vertical slice: spawn the capsid host
// per fixture, drive a real WebSocket client, and check the /selftest
// contracts. Until then this branch fails loudly rather than silently
// passing.
assert.fail(
    'WebSocketPair is in the profile but the runner harness is not ' +
    'implemented yet - land tests/test_workerd_ws_fixtures.mjs runner ' +
    'support together with the vertical slice');

import assert from 'node:assert/strict';

// Shared with negative controls: inspect the actual gate without spawning
// extra full auditors inside the existing 30-second workflow-control budget.
export function auditReleasePackages(releaseWorkflow) {
  const packageMatrix = releaseWorkflow.match(
    /      matrix:\r?\n        include:\r?\n([\s\S]*?)    runs-on:/,
  )?.[1];
  assert.ok(packageMatrix, 'release has an explicit reviewed package matrix');
  const packageRows = [
    ...packageMatrix.matchAll(/^ {10}- os: ([^\r\n]+)\r?\n((?: {12}[^\r\n]+\r?\n)*)/gm),
  ].map((match) => {
    const row = { os: match[1] };
    for (const field of ['package_platform', 'native_machine', 'docker_platform', 'macos_arch']) {
      const value = match[2].match(new RegExp(`^ {12}${field}: ([^\\r\\n]+)`, 'm'));
      if (value) row[field] = value[1];
    }
    return row;
  });
  assert.deepEqual(
    packageRows,
    [
      {
        os: 'ubuntu-24.04',
        package_platform: 'linux-musl-x86_64',
        native_machine: 'x86_64',
        docker_platform: 'linux/amd64',
      },
      {
        os: 'ubuntu-24.04-arm',
        package_platform: 'linux-musl-arm64',
        native_machine: 'aarch64',
        docker_platform: 'linux/arm64',
      },
      {
        os: 'macos-15',
        package_platform: 'darwin-arm64',
        native_machine: 'arm64',
        macos_arch: 'arm64',
      },
      {
        os: 'macos-15-intel',
        package_platform: 'darwin-x86_64',
        native_machine: 'x86_64',
        macos_arch: 'x86_64',
      },
      { os: 'windows-latest', package_platform: 'windows-x86_64' },
    ],
    'release binds each distinct archive platform to its native runner and container',
  );
  function releaseStep(name) {
    const blocks = releaseWorkflow.split(`      - name: ${name}`);
    assert.equal(blocks.length, 2, `one required release step: ${name}`);
    return blocks[1].split(/\r?\n      - name:/)[0];
  }
  const linuxPackage = releaseStep('Build and package in native Alpine (musl, fully static)');
  assert.match(linuxPackage, /if: runner\.os == 'Linux'/, 'both Linux runners build musl packages');
  assert.equal(
    linuxPackage.split('test "$(uname -m)" = "${EXPECTED_MACHINE}"').length - 1,
    2,
    'native CPU is checked on host and in container',
  );
  for (const fragment of [
    'EXPECTED_MACHINE: ${{ matrix.native_machine }}',
    'DOCKER_PLATFORM: ${{ matrix.docker_platform }}',
    '--platform "${DOCKER_PLATFORM}"',
    'package_platform_naming|release_package_architecture_controls|worker_package_contents|worker_package_smoke',
  ])
    assert.ok(linuxPackage.includes(fragment), `Linux package gate retains ${fragment}`);
  const macOpenSSL = releaseStep('Build OpenSSL 3.5.7 for macOS Release');
  const macPackage = releaseStep('Configure and build macOS Release/LTO');
  for (const block of [macOpenSSL, macPackage]) {
    assert.match(
      block,
      /if: runner\.os == 'macOS'/,
      'both native macOS CPU jobs execute the complete build',
    );
  }
  for (const fragment of [
    'EXPECTED_MACHINE: ${{ matrix.native_machine }}',
    'MACOS_ARCH: ${{ matrix.macos_arch }}',
    'test "$(uname -m)" = "${EXPECTED_MACHINE}"',
    '-DCMAKE_OSX_ARCHITECTURES="${MACOS_ARCH}"',
    'ctest --test-dir build --no-tests=error --output-on-failure',
    'package_platform_naming|release_package_architecture_controls|worker_package_contents|worker_package_smoke',
  ])
    assert.ok(macPackage.includes(fragment), `macOS package gate retains ${fragment}`);
  const packageVerify = releaseStep('Verify native package before upload');
  assert.doesNotMatch(
    packageVerify,
    /^\s+(?:if:|continue-on-error:)/m,
    'every package is verified without bypass',
  );
  assert.ok(
    packageVerify.includes('CAPSID_PACKAGE_PLATFORM: ${{ matrix.package_platform }}'),
    'package verifier binds the expected architecture',
  );
  assert.ok(
    packageVerify.includes('python tools/verify-release-package.py'),
    'package verification executes the actual verifier',
  );
  assert.match(
    releaseWorkflow,
    /^  workflow_dispatch:\s*$/m,
    'manual package preflight is available',
  );
  const publishPackage = releaseStep('Publish package to GitHub Release');
  assert.ok(
    publishPackage.includes(
      "github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v') && !contains(github.ref_name, '-')",
    ),
    'manual preflight cannot publish or edit a Release',
  );
}

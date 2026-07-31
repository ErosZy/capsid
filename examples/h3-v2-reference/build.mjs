import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const referenceRoot = path.dirname(fileURLToPath(import.meta.url));
const defaultEsbuild = path.resolve(
    referenceRoot,
    '../../vendor/txiki.js/node_modules/.bin/esbuild',
);

const options = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    options.set(process.argv[index], process.argv[index + 1]);
}

const buildOne = ({ entry, outfile, metafile, esbuild }) => {
    mkdirSync(path.dirname(outfile), { recursive: true });
    mkdirSync(path.dirname(metafile), { recursive: true });
    const result = spawnSync(esbuild, [
        entry,
        '--bundle',
        '--minify',
        '--keep-names',
        '--tree-shaking=true',
        '--target=esnext',
        '--platform=neutral',
        '--format=esm',
        `--metafile=${metafile}`,
        `--outfile=${outfile}`,
    ], {
        cwd: referenceRoot,
        stdio: 'inherit',
    });

    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(`esbuild exited with status ${result.status}`);
    }
};

if (options.size > 0) {
    const entry = options.get('--entry');
    const outfile = options.get('--outfile');
    const metafile = options.get('--metafile');
    const esbuild = options.get('--esbuild') ?? defaultEsbuild;
    if (!entry || !outfile || !metafile) {
        throw new Error('expected --entry, --outfile and --metafile');
    }
    buildOne({ entry, outfile, metafile, esbuild });
} else {
    const outputRoot = path.join(referenceRoot, 'dist');
    for (const [ name, entry ] of [
        [ 'default-app', 'entry-default-app.js' ],
        [ 'default-object', 'entry-default-object.js' ],
        [ 'named', 'entry-named.js' ],
        [ 'wrapper', 'entry-wrapper.js' ],
        [ 'handler', 'entry-handler.js' ],
        [ 'malformed', 'entry-malformed.js' ],
        [ 'debug', 'entry-debug.js' ],
    ]) {
        buildOne({
            entry: path.join(referenceRoot, 'src', entry),
            outfile: path.join(outputRoot, `${name}.mjs`),
            metafile: path.join(outputRoot, `${name}.meta.json`),
            esbuild: defaultEsbuild,
        });
    }
}

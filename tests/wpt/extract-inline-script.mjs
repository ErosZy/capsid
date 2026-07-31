import fs from 'node:fs';

const [ inputPath, outputPath ] = process.argv.slice(2);

if (!inputPath || !outputPath) {
    throw new Error('expected input and output paths');
}

const html = fs.readFileSync(inputPath, 'utf8');
const scripts = [];
const pattern = /<script(?![^>]*\bsrc\s*=)[^>]*>([\s\S]*?)<\/script>/gi;
let match;

while ((match = pattern.exec(html)) !== null) {
    scripts.push(match[1]);
}

if (scripts.length === 0) {
    throw new Error(`no inline scripts found in ${inputPath}`);
}

fs.writeFileSync(outputPath, scripts.join('\n'));

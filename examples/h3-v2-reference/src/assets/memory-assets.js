const encoder = new TextEncoder();

const assets = new Map([
    [ '/utilities/static/hello.txt', {
        bytes: encoder.encode('hello from in-memory H3 static'),
        type: 'text/plain; charset=utf-8',
        etag: '"memory-hello-v1"',
        mtime: '2024-01-02T03:04:05.000Z',
    } ],
    [ '/utilities/static/index.html', {
        bytes: encoder.encode('<h1>H3 memory index</h1>'),
        type: 'text/html; charset=utf-8',
        etag: '"memory-index-v1"',
        mtime: '2024-01-02T03:04:05.000Z',
    } ],
]);

export const memoryStaticOptions = {
    indexNames: [ '/index.html' ],
    headers: { 'x-static-source': 'memory' },
    getMeta(id) {
        const asset = assets.get(id);
        return asset ? {
            type: asset.type,
            etag: asset.etag,
            mtime: asset.mtime,
            size: asset.bytes.byteLength,
        } : undefined;
    },
    getContents(id) {
        return assets.get(id)?.bytes;
    },
};

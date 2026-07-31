async function decompressDataOrPako(chunk, format) {
    const stream = new DecompressionStream(format);
    const writer = stream.writable.getWriter();
    const writePromise = writer.write(chunk);
    const closePromise = writer.close();

    const reader = stream.readable.getReader();
    const chunks = [];
    let length = 0;
    while (true) {
        const { value, done } = await reader.read();
        if (done) {
            break;
        }
        chunks.push(value);
        length += value.byteLength;
    }
    const output = new Uint8Array(length);
    let offset = 0;
    for (const value of chunks) {
        output.set(value, offset);
        offset += value.byteLength;
    }
    await writePromise;
    await closePromise;
    return output;
}

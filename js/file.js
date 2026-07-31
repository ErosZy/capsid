const NativeFile = globalThis.File;

class File extends NativeFile {
    constructor(fileBits, fileName, options = {}) {
        if (arguments.length < 2) {
            throw new TypeError(
                `Failed to construct 'File': 2 arguments required, but only ` +
                `${arguments.length} present.`);
        }
        if (options === null) {
            options = {};
        }
        super(fileBits, fileName, options);
    }
}

Object.defineProperty(File.prototype, Symbol.toStringTag, {
    configurable: true,
    value: 'File',
});
Object.defineProperty(globalThis, 'File', {
    configurable: true,
    enumerable: false,
    writable: true,
    value: File,
});

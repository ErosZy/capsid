const NativeWebAssembly = globalThis.WebAssembly;

if (NativeWebAssembly) {
    const NativeModule = NativeWebAssembly.Module;
    const NativeMemory = NativeWebAssembly.Memory;
    const getNativeMemoryBuffer =
        Object.getOwnPropertyDescriptor(
            NativeMemory.prototype,
            'buffer',
        ).get;
    const nativeValidate = NativeWebAssembly.validate;
    const nativeInstantiate = NativeWebAssembly.instantiate;

    function copyBufferSource(source) {
        if (source instanceof ArrayBuffer) {
            return new Uint8Array(source.slice(0));
        }
        if (ArrayBuffer.isView(source)) {
            return new Uint8Array(
                source.buffer,
                source.byteOffset,
                source.byteLength,
            ).slice();
        }
        throw new TypeError('Argument 0 must be a BufferSource');
    }

    function toU32(value, label) {
        const number = Number(value);
        const integer = Math.trunc(number);

        if (!Number.isFinite(number) || integer < 0 || integer > 0xffffffff) {
            throw new TypeError(`${label} must be an unsigned long`);
        }
        return integer;
    }

    class Module extends NativeModule {
        constructor(source) {
            super(copyBufferSource(source));
        }
    }

    class Memory extends NativeMemory {
        constructor(descriptor) {
            if ((typeof descriptor !== 'object' &&
                 typeof descriptor !== 'function') ||
                descriptor === null) {
                throw new TypeError(
                    'WebAssembly.Memory(): Argument 0 must be a memory descriptor');
            }

            const initialValue = descriptor.initial;
            if (initialValue === undefined) {
                throw new TypeError(
                    'WebAssembly.Memory(): Property \'initial\' is required');
            }
            const initial = toU32(initialValue, 'initial');
            const maximumValue = descriptor.maximum;
            const normalized = { initial };

            if (maximumValue !== undefined) {
                normalized.maximum = toU32(maximumValue, 'maximum');
            }
            if (descriptor.shared !== undefined) {
                normalized.shared = Boolean(descriptor.shared);
            }
            super(normalized);
        }

        grow(delta) {
            // Perform the native/private brand check before coercing delta.
            // The WebAssembly API must not touch the argument for an
            // incompatible receiver.
            getNativeMemoryBuffer.call(this);
            const oldBuffer = this.buffer;
            const result = super.grow(toU32(delta, 'delta'));
            const newBuffer = this.buffer;

            if (oldBuffer !== newBuffer &&
                oldBuffer instanceof ArrayBuffer &&
                oldBuffer.byteLength !== 0) {
                structuredClone(oldBuffer, { transfer: [ oldBuffer ] });
            }
            return result;
        }
    }

    function validate(source) {
        return nativeValidate.call(
            NativeWebAssembly,
            copyBufferSource(source),
        );
    }

    function compile(source) {
        let bytes;

        try {
            bytes = copyBufferSource(source);
        } catch (error) {
            return Promise.reject(error);
        }

        return Promise.resolve().then(() => {
            try {
                return new Module(bytes);
            } catch (error) {
                if (error instanceof TypeError) {
                    throw new NativeWebAssembly.CompileError(error.message);
                }
                throw error;
            }
        });
    }

    function instantiate(source, imports = undefined) {
        const importsProvided =
            arguments.length >= 2 && imports !== undefined;

        if (source instanceof NativeModule) {
            return importsProvided ?
                nativeInstantiate.call(
                    NativeWebAssembly,
                    source,
                    imports,
                ) :
                nativeInstantiate.call(NativeWebAssembly, source);
        }

        let bytes;
        try {
            bytes = copyBufferSource(source);
        } catch (error) {
            return Promise.reject(error);
        }

        return importsProvided ?
            nativeInstantiate.call(NativeWebAssembly, bytes, imports) :
            nativeInstantiate.call(NativeWebAssembly, bytes);
    }

    NativeWebAssembly.Module = Module;
    NativeWebAssembly.Memory = Memory;
    NativeWebAssembly.validate = validate;
    NativeWebAssembly.compile = compile;
    NativeWebAssembly.instantiate = instantiate;
}

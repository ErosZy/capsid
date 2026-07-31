export const delay = milliseconds => new Promise(resolve => {
    setTimeout(resolve, milliseconds);
});

export const toHex = value => {
    let output = '';
    for (const byte of new Uint8Array(value)) {
        output += byte.toString(16).padStart(2, '0');
    }
    return output;
};

export const bodyChecksum = bytes => {
    let checksum = 0;
    for (const byte of bytes) {
        checksum = (checksum + byte) >>> 0;
    }
    return checksum;
};

export const formDataObject = async form => {
    const output = Object.create(null);
    for (const [ key, value ] of form.entries()) {
        const normalized = value instanceof File ? {
            name: value.name,
            type: value.type,
            size: value.size,
            text: await value.text(),
        } : value;
        if (Object.hasOwn(output, key)) {
            output[key] = Array.isArray(output[key])
                ? [ ...output[key], normalized ]
                : [ output[key], normalized ];
        } else {
            output[key] = normalized;
        }
    }
    return output;
};

export const requiredFieldSchema = field => ({
    '~standard': {
        version: 1,
        vendor: 'capsid-runtime-reference',
        validate(value) {
            if (
                value &&
                typeof value === 'object' &&
                typeof value[field] === 'string' &&
                value[field].length > 0
            ) {
                return { value };
            }
            return {
                issues: [ {
                    message: `${field} must be a non-empty string`,
                    path: [ field ],
                } ],
            };
        },
    },
});

export const traceFor = (event, name) => {
    const key = `${name}Trace`;
    event.context[key] ??= [];
    return event.context[key];
};

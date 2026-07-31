// QuickJS-ng provides the DOMException implementation and brand checks, but
// its Web IDL attributes are not enumerable. Web IDL requires interface
// prototype attributes to be enumerable.
for (const name of [ 'name', 'message', 'code' ]) {
    const descriptor = Object.getOwnPropertyDescriptor(
        DOMException.prototype, name);

    if (descriptor) {
        Object.defineProperty(DOMException.prototype, name, {
            ...descriptor,
            enumerable: true,
        });
    }
}

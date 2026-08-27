#include "quickjs.h"

#include <cstdio>
#include <cstring>

namespace {

void print_exception(JSContext* ctx) {
    JSValue exception = JS_GetException(ctx);
    const char* text = JS_ToCString(ctx, exception);
    std::fprintf(stderr, "%s\n", text ? text : "<exception>");
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, exception);
}

}  // namespace

int main() {
    static constexpr char source[] = R"JS(
function assert(condition, message) {
  if (!condition) throw new Error(message);
}
function same(actual, expected, message) {
  const left = JSON.stringify(actual);
  const right = JSON.stringify(expected);
  if (left !== right) throw new Error(`${message}: ${left} !== ${right}`);
}

same([0, 1, 2, 3].slice(1, 3), [1, 2], 'dense slice');
same([0, 1, 2, 3].slice(-2), [2, 3], 'negative slice');
same([0, 1].slice(), [0, 1], 'slice without arguments');

const hole = [0, , 2];
const holeCopy = hole.slice();
assert(!(1 in holeCopy), 'slice must preserve a true hole');
Array.prototype[1] = 9;
const inheritedCopy = hole.slice();
delete Array.prototype[1];
same(inheritedCopy, [0, 9, 2], 'slice must read inherited indices');
assert(Object.hasOwn(inheritedCopy, 1), 'inherited value must become own');

let sliceSpeciesCalls = 0;
class SliceResult extends Array {}
class SliceSource extends Array {
  static get [Symbol.species]() {
    sliceSpeciesCalls++;
    return SliceResult;
  }
}
const speciesSlice = new SliceSource(1, 2, 3).slice(1);
assert(speciesSlice instanceof SliceResult, 'slice must honor species');
same(speciesSlice, [2, 3], 'species slice contents');
assert(sliceSpeciesCalls === 1, 'slice must read species once');

const shrink = [0, 1, 2, 3, 4];
same(shrink.splice(1, 3, 8), [1, 2, 3], 'shrink deleted values');
same(shrink, [0, 8, 4], 'shrink receiver');

const grow = [0, 1, 2, 3];
same(grow.splice(1, 1, 7, 8, 9), [1], 'grow deleted values');
same(grow, [0, 7, 8, 9, 2, 3], 'grow receiver');

const equal = [0, 1, 2, 3];
same(equal.splice(1, 2, 8, 9), [1, 2], 'equal deleted values');
same(equal, [0, 8, 9, 3], 'equal receiver');

const noArgs = [0, 1, 2];
same(noArgs.splice(), [], 'splice without arguments deletes nothing');
same(noArgs, [0, 1, 2], 'splice without arguments receiver');
const oneArg = [0, 1, 2];
same(oneArg.splice(1), [1, 2], 'one-argument splice deletes tail');
same(oneArg, [0], 'one-argument splice receiver');

let spliceSpeciesCalls = 0;
class SpliceResult extends Array {}
class SpliceSource extends Array {
  static get [Symbol.species]() {
    spliceSpeciesCalls++;
    return SpliceResult;
  }
}
const speciesSource = new SpliceSource(0, 1, 2);
const speciesDeleted = speciesSource.splice(1, 1, 7, 8);
assert(speciesDeleted instanceof SpliceResult, 'splice must honor species');
same(speciesDeleted, [1], 'species splice deleted values');
same(speciesSource, [0, 7, 8, 2], 'species splice receiver');
assert(spliceSpeciesCalls === 1, 'splice must read species once');

const sparse = [0, , 2, 3];
const sparseDeleted = sparse.splice(0, 3, 8);
assert(!(1 in sparseDeleted), 'splice must preserve deleted hole');
same(sparse, [8, 3], 'sparse splice receiver');

const locked = [0, 1, 2];
Object.defineProperty(locked, 'length', { writable: false });
let lockedThrew = false;
try {
  locked.splice(1, 1, 8);
} catch (error) {
  lockedThrew = error instanceof TypeError;
}
assert(lockedThrew, 'non-writable length must throw');

const nonextensible = [0, 1, 2];
Object.preventExtensions(nonextensible);
same(nonextensible.splice(1, 1, 8), [1],
     'nonextensible equal splice deleted values');
same(nonextensible, [0, 8, 2], 'nonextensible equal splice receiver');

const proxyLog = [];
const proxyTarget = [0, 1, 2];
const proxy = new Proxy(proxyTarget, {
  get(target, key, receiver) {
    proxyLog.push(`get:${String(key)}`);
    return Reflect.get(target, key, receiver);
  },
  set(target, key, value, receiver) {
    proxyLog.push(`set:${String(key)}`);
    return Reflect.set(target, key, value, receiver);
  },
  deleteProperty(target, key) {
    proxyLog.push(`delete:${String(key)}`);
    return Reflect.deleteProperty(target, key);
  },
});
same(Array.prototype.splice.call(proxy, 1, 1, 9, 8), [1],
     'proxy splice deleted values');
same(proxyTarget, [0, 9, 8, 2], 'proxy splice receiver');
assert(proxyLog.includes('get:length'), 'proxy must observe length get');
assert(proxyLog.includes('set:length'), 'proxy must observe length set');

for (let round = 0; round < 2000; round++) {
  const values = [];
  for (let index = 0; index < 32; index++) values.push({ round, index });
  const removed = values.splice(4, 20, { round }, { round });
  assert(removed.length === 20 && removed[0].index === 4,
         'shrink ownership stress');
  values.splice(2, 1, ...removed.slice(0, 12));
  assert(values[2].index === 4 && values[13].index === 15,
         'grow ownership stress');
}
)JS";

    JSRuntime* runtime = JS_NewRuntime();
    JSContext* context = runtime ? JS_NewContext(runtime) : nullptr;
    if (!runtime || !context)
        return 2;

    JSValue result = JS_Eval(context, source, std::strlen(source),
                             "array-fast-paths.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        print_exception(context);
        JS_FreeValue(context, result);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        return 1;
    }
    JS_FreeValue(context, result);
    JS_RunGC(runtime);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    std::fprintf(stderr, "test_array_fast_paths: all green\n");
    return 0;
}

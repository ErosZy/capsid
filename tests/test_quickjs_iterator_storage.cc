extern "C" {
#include "quickjs.h"
}
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
struct Allocator {
    size_t live = 0, bytes = 0, attempts = 0, fail_at = 0, failures = 0;
};
struct alignas(std::max_align_t) Header { size_t size; Allocator *owner; };
bool fail(Allocator *a) {
    ++a->attempts;
    if (a->fail_at != 0 && a->attempts == a->fail_at) { ++a->failures; return true; }
    return false;
}
void *allocate(void *opaque, size_t size) {
    Allocator *a = static_cast<Allocator *>(opaque);
    if (size > std::numeric_limits<size_t>::max() - sizeof(Header) || fail(a)) return NULL;
    Header *h = static_cast<Header *>(std::malloc(sizeof(Header) + size));
    if (!h) return NULL;
    h->size = size; h->owner = a; ++a->live; a->bytes += size;
    return h + 1;
}
void *allocate_zero(void *opaque, size_t count, size_t size) {
    if (count != 0 && size > std::numeric_limits<size_t>::max() / count) return NULL;
    void *p = allocate(opaque, count * size);
    if (p) std::memset(p, 0, count * size);
    return p;
}
void release(void *opaque, void *p) {
    if (!p) return;
    Header *h = static_cast<Header *>(p) - 1;
    Allocator *a = static_cast<Allocator *>(opaque);
    if (h->owner != a || a->live == 0 || h->size > a->bytes) std::abort();
    --a->live; a->bytes -= h->size; std::free(h);
}
void *resize(void *opaque, void *p, size_t size) {
    if (!p) return allocate(opaque, size);
    if (size == 0) { release(opaque, p); return NULL; }
    Allocator *a = static_cast<Allocator *>(opaque);
    if (size > std::numeric_limits<size_t>::max() - sizeof(Header) || fail(a)) return NULL;
    Header *old = static_cast<Header *>(p) - 1;
    if (old->owner != a) std::abort();
    const size_t previous = old->size;
    Header *h = static_cast<Header *>(std::realloc(old, sizeof(Header) + size));
    if (!h) return NULL;
    h->size = size; a->bytes = a->bytes - previous + size;
    return h + 1;
}
size_t usable(const void *p) { return p ? (static_cast<const Header *>(p) - 1)->size : 0; }
const JSMallocFunctions functions = {allocate_zero, allocate, release, resize, usable};
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Runtime {
    JSRuntime *rt;
    JSContext *ctx;
    explicit Runtime(Allocator *a) : rt(JS_NewRuntime2(&functions, a)), ctx(NULL) {
        require(rt != NULL, "runtime allocation");
        ctx = JS_NewContext(rt);
        if (!ctx) { JS_FreeRuntime(rt); rt = NULL; throw std::runtime_error("context allocation"); }
        JS_SetGCThreshold(rt, std::numeric_limits<size_t>::max());
    }
    ~Runtime() { if (ctx) JS_FreeContext(ctx); if (rt) { JS_RunGC(rt); JS_FreeRuntime(rt); } }
};
void clear_exception(JSContext *ctx) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); }
void eval(JSContext *ctx, const char *code) {
    JSValue value = JS_Eval(ctx, code, std::strlen(code), "iterator-storage-properties.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue ex = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, ex);
        std::fprintf(stderr, "JavaScript property failure: %s\n", message ? message : "unprintable");
        JS_FreeCString(ctx, message); JS_FreeValue(ctx, ex);
        throw std::runtime_error("iterator observable behavior");
    }
    require(JS_ToBool(ctx, value) == 1, "JavaScript property did not complete");
    JS_FreeValue(ctx, value);
}
size_t property_oom(size_t point, size_t *failure_count) {
    Allocator a;
    size_t operations = 0;
    {
        Runtime vm(&a);
        JSValue array = JS_NewArray(vm.ctx);
        require(!JS_IsException(array), "array setup");
        JSValue method = JS_GetPropertyStr(vm.ctx, array, "values");
        require(!JS_IsException(method), "iterator method setup");
        const size_t start = a.attempts;
        if (point) a.fail_at = start + point;
        JSValue iterator = JS_Call(vm.ctx, method, array, 0, NULL);
        bool failed = JS_IsException(iterator);
        if (!failed) {
            for (unsigned i = 0; i < 21; ++i) {
                const std::string key = "owned" + std::to_string(i);
                if (JS_SetPropertyStr(vm.ctx, iterator, key.c_str(), JS_NewInt32(vm.ctx, static_cast<int>(i))) < 0) {
                    failed = true; break;
                }
            }
        }
        operations = a.attempts - start;
        a.fail_at = 0;
        if (failed) clear_exception(vm.ctx);
        JS_FreeValue(vm.ctx, iterator); JS_FreeValue(vm.ctx, method); JS_FreeValue(vm.ctx, array);
        JS_RunGC(vm.rt);
        *failure_count = a.failures;
    }
    require(a.live == 0 && a.bytes == 0, "allocation failure leaked VM or iterator storage");
    return operations;
}
}

int main() {
    try {
        Allocator a;
        size_t iterator_allocations = 0;
        {
            Runtime vm(&a);
            eval(vm.ctx, R"JS((() => {
                const check = (ok, label) => { if (!ok) throw new Error(label); };
                const equal = (a,b,label) => check(JSON.stringify(a) === JSON.stringify(b), label);
                equal([...['a','b'].values()], ['a','b'], 'values');
                equal([...['a','b'].keys()], [0,1], 'keys');
                equal([...['a','b'].entries()], [[0,'a'],[1,'b']], 'entries');
                const array = [10,20], iterator = array.values();
                check(iterator[Symbol.iterator]() === iterator, 'iterator identity');
                check(Object.isExtensible(iterator), 'initial extensibility');
                check(Reflect.ownKeys(iterator).length === 0, 'initial own properties');
                for (let i=0;i<64;i++) iterator['extra'+i] = i;
                for (let i=0;i<48;i++) delete iterator['extra'+i];
                Object.defineProperty(iterator, 'answer', {get(){return 42}, configurable:true});
                const symbol = Symbol('own'); iterator[symbol] = 77;
                check(iterator.answer === 42 && iterator[symbol] === 77 && iterator.extra63 === 63, 'own property growth and compaction');
                check(iterator.next().value === 10, 'state survives own properties');
                array.push(30); delete array[1];
                equal([iterator.next().value, iterator.next().value], [undefined,30], 'live mutation and hole');
                check(iterator.next().done && iterator.next().done, 'permanent completion');
                const frozen = [8].values(); Object.freeze(frozen);
                check(frozen.next().value === 8 && frozen.next().done, 'freeze does not freeze internal progress');
                const strings = 'a\uD83D\uDE00\uD800b'[Symbol.iterator]();
                strings.extra = 9;
                equal([...strings], ['a','\uD83D\uDE00','\uD800','b'], 'string code points and own property');
                const raw = [4,5].values(), next = raw.next;
                Object.setPrototypeOf(raw, null); raw.own = 6;
                check(next.call(raw).value === 4 && raw.own === 6, 'prototype change preserves state');
                let closed = 0;
                const proto = Object.getPrototypeOf([0].values());
                const old = Object.getOwnPropertyDescriptor(proto,'return');
                Object.defineProperty(proto,'return',{configurable:true,value:function(){
                    check(next.call(this).value === 2, 'close receives actual iterator'); this.closed = true; closed++; return {};
                }});
                try { for (const value of [1,2,3]) { check(value===1, 'first value'); break; } }
                finally { if (old) Object.defineProperty(proto,'return',old); else delete proto.return; }
                check(closed === 1, 'close called once');
                const buffer = new ArrayBuffer(8), typed = new Uint8Array(buffer).values();
                buffer.transfer(); let detached = false;
                try { typed.next(); } catch (e) { detached = e instanceof TypeError; }
                check(detached, 'detached typed array keeps TypeError');
                return true;
            })())JS");
            for (unsigned round = 0; round < 16; ++round) {
                eval(vm.ctx, R"JS((() => {
                    for (let i=0;i<128;i++) {
                        const a = [], it = a.entries(); a.push(it); it.owner = a;
                        const string = 'x\uD83D\uDE00'[Symbol.iterator](); string.peer = it; it.peer = string;
                    }
                    return true;
                })())JS");
                JS_RunGC(vm.rt);
            }
            JSValue array = JS_NewArray(vm.ctx);
            JSValue method = JS_GetPropertyStr(vm.ctx, array, "values");
            JSValue warm = JS_Call(vm.ctx, method, array, 0, NULL);
            require(!JS_IsException(warm), "retained warm iterator");
            JSMemoryUsage before = {}, after = {};
            JS_ComputeMemoryUsage(vm.rt, &before);
            const size_t live = a.live, bytes = a.bytes;
            JSValue measured = JS_Call(vm.ctx, method, array, 0, NULL);
            require(!JS_IsException(measured), "measured iterator");
            iterator_allocations = a.live - live;
            const size_t added_bytes = a.bytes - bytes;
            JS_ComputeMemoryUsage(vm.rt, &after);
            JS_FreeValue(vm.ctx, measured); JS_FreeValue(vm.ctx, warm); JS_FreeValue(vm.ctx, method); JS_FreeValue(vm.ctx, array);
            require(iterator_allocations == 1, "empty iterator still requires separate allocations");
            require(after.obj_count - before.obj_count == 1, "iterator object metric");
            require(after.memory_used_count - before.memory_used_count == static_cast<int64_t>(iterator_allocations), "iterator allocation metric");
            require(after.memory_used_size - before.memory_used_size == static_cast<int64_t>(added_bytes), "iterator byte metric omits inline state");
        }
        require(a.live == 0 && a.bytes == 0, "observable or cyclic iterator leaked");
        size_t none = 0;
        const size_t operations = property_oom(0, &none);
        require(operations > 4 && none == 0, "allocation sweep setup");
        for (size_t point = 1; point <= operations; ++point) {
            size_t failures = 0;
            property_oom(point, &failures);
            require(failures == 1, "allocation failure point not exercised");
        }
        std::printf("PASS: iterator observables, 2048 cyclic graphs, exact memory metrics, %zu allocation failure points; empty iterator allocations=%zu\n", operations, iterator_allocations);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}

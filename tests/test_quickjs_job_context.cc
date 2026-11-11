// WP-02 §6.5 gate: QuickJS job-context hook semantics.
//
// RED: this file does not compile before 0012-capsid-async-context.patch
//      (JSJobContextHooks / JS_SetJobContextHooks do not exist yet).
// GREEN: every §6.5 scenario passes:
//   1. capture exactly once per successfully enqueued job (at enqueue time)
//   2. enter before job_func / leave after (all return paths) / release once
//   3. JS_FreeRuntime releases each unexecuted job exactly once
//   4. nested job drain: enter(B) observes previous=A, leave restores A,
//      exit restores null
//   5. capture failure fails enqueue closed (no pending job, runtime usable)
//   6. hooks not installed -> upstream behavior, zero callbacks
//   7. promise reactions capture the .then() creation context; a promise
//      settled from a different job's context still runs its reaction under
//      the creation context (0014-capsid-async-context-reactions.patch)

#include "quickjs.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

struct HookLog {
    int captures = 0;
    int enters = 0;
    int leaves = 0;
    int releases = 0;
    int fail_captures = 0;   // >0: next captures return -1
    int next_tag = 0;
    void *active = nullptr;  // test's view of the current job context
    std::vector<std::string> sequence;
    int origin_captures = 0;
    int origin_releases = 0;
    bool fail_origin_capture = false;
    int fail_origin_after = -1;
    std::map<void *, int> active_job_captures;
    std::set<void *> revoked_origins;
};

int capture_hook(JSContext *ctx, void *opaque, void **out_job_context) {
    auto *log = static_cast<HookLog *>(opaque);
    ++log->captures;
    if (log->fail_captures > 0) {
        --log->fail_captures;
        return -1;
    }
    if (log->active != nullptr) {
        // A capture inside a running job (e.g. a promise reaction created
        // while another job is active) inherits the enclosing job's context
        // — the creation-time capture the worker relies on.
        *out_job_context = log->active;
        ++log->active_job_captures[*out_job_context];
        return 0;
    }
    *out_job_context =
        reinterpret_cast<void *>(static_cast<uintptr_t>(++log->next_tag));
    ++log->active_job_captures[*out_job_context];
    return 0;
}

void *enter_hook(JSContext *ctx, void *job_context, void *opaque) {
    auto *log = static_cast<HookLog *>(opaque);
    ++log->enters;
    void *previous = log->active;
    log->active = job_context;
    log->sequence.push_back(
        "enter:" +
        std::to_string(reinterpret_cast<uintptr_t>(job_context)));
    return previous;
}

void leave_hook(JSContext *ctx, void *previous_context, void *opaque) {
    auto *log = static_cast<HookLog *>(opaque);
    ++log->leaves;
    log->active = previous_context;
    log->sequence.push_back(
        "leave:" +
        std::to_string(reinterpret_cast<uintptr_t>(previous_context)));
}

void release_hook(void *job_context, void *opaque) {
    auto *log = static_cast<HookLog *>(opaque);
    ++log->releases;
    --log->active_job_captures[job_context];
    log->sequence.push_back(
        "release:" +
        std::to_string(reinterpret_cast<uintptr_t>(job_context)));
}

JSValue noop_job(JSContext *ctx, int argc, JSValueConst *argv) {
    return JS_UNDEFINED;
}

JSValue throw_job(JSContext *ctx, int argc, JSValueConst *argv) {
    return JS_Throw(ctx, JS_NewString(ctx, "job boom"));
}

// Jobs that observe the job context while running. Each carries its
// expectation in argv[0] (an int64 holding a JobState pointer), so jobs
// stay independent of the context opaque, which is the shared HookLog.
struct JobState {
    HookLog *log;
    void *expected_active;
};

JobState *state_from_arg(JSContext *ctx, JSValueConst argv) {
    int64_t state_value = 0;
    require(JS_ToInt64(ctx, &state_value, argv) == 0,
            "job state argument is not an int64");
    return reinterpret_cast<JobState *>(static_cast<intptr_t>(state_value));
}

JSValue active_check_job(JSContext *ctx, int argc, JSValueConst *argv) {
    require(argc == 1, "active_check_job requires a state argument");
    auto *state = state_from_arg(ctx, argv[0]);
    auto *log = static_cast<HookLog *>(JS_GetContextOpaque(ctx));
    require(log->active == state->expected_active,
            "job did not observe its own job context");
    return JS_UNDEFINED;
}

// A job that drains the pending queue while running; it must observe its
// own context as active both before and after the nested drain.
JSValue nested_drain_job(JSContext *ctx, int argc, JSValueConst *argv) {
    require(argc == 1, "nested_drain_job requires a state argument");
    auto *state = state_from_arg(ctx, argv[0]);
    auto *log = static_cast<HookLog *>(JS_GetContextOpaque(ctx));
    require(log->active == state->expected_active,
            "nested job start: own context not active");
    JSContext *sub_ctx = nullptr;
    require(JS_ExecutePendingJob(JS_GetRuntime(ctx), &sub_ctx) == 1,
            "nested drain did not execute the inner job");
    require(log->active == state->expected_active,
            "nested drain: outer context not restored after inner job");
    return JS_UNDEFINED;
}

void enqueue_with_state(JSContext *ctx, JSJobFunc *job, JobState *state) {
    JSValue arg = JS_NewInt64(
        ctx, static_cast<int64_t>(reinterpret_cast<intptr_t>(state)));
    require(JS_EnqueueJob(ctx, job, 1, &arg) == 0, "enqueue failed");
    JS_FreeValue(ctx, arg);  // enqueue dups internally; caller owns its ref
}

// A pending promise with a .then() reaction created inside a job, then
// settled from a different job. The reaction job must run under the
// creation context (the .then() caller's), not the settle job's context.
const char kPromiseSetupScript[] =
    "globalThis.__p = new Promise((resolve) => {"
    "  globalThis.__resolveP = resolve;"
    "});"
    "globalThis.__r = globalThis.__p.then(() => {"
    "  globalThis.__observed = true;"
    "});"
    "0";
const char kPromiseSettleScript[] =
    "globalThis.__resolveP(42);"
    "0";
const char kPromiseObservedScript[] =
    "globalThis.__observed === true ? 1 : 0";

JSValue promise_setup_job(JSContext *ctx, int argc, JSValueConst *argv) {
    JSValue result = JS_Eval(ctx, kPromiseSetupScript,
                             sizeof(kPromiseSetupScript) - 1,
                             "<promise-capture>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, result);
    return JS_UNDEFINED;
}

JSValue promise_settle_job(JSContext *ctx, int argc, JSValueConst *argv) {
    JSValue result = JS_Eval(ctx, kPromiseSettleScript,
                             sizeof(kPromiseSettleScript) - 1,
                             "<promise-settle>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, result);
    return JS_UNDEFINED;
}

const JSJobContextHooks kHooks = {
    capture_hook,
    enter_hook,
    leave_hook,
    release_hook,
};

struct TestRuntime {
    JSRuntime *rt;
    JSContext *ctx;
    HookLog log;
    TestRuntime() : rt(JS_NewRuntime()), ctx(JS_NewContext(rt)) {
        JS_SetContextOpaque(ctx, &log);
    }
    ~TestRuntime() {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
};

void run_default_behavior() {
    // §6.1: hooks not installed -> layout-out behavior identical, zero calls.
    JSRuntime *plain_rt = JS_NewRuntime();
    JSContext *plain_ctx = JS_NewContext(plain_rt);
    require(JS_EnqueueJob(plain_ctx, noop_job, 0, nullptr) == 0,
            "default enqueue failed");
    require(JS_IsJobPending(plain_rt), "default enqueue left no pending job");
    JSContext *exec_ctx = nullptr;
    require(JS_ExecutePendingJob(plain_rt, &exec_ctx) == 1,
            "default execute failed");
    require(!JS_IsJobPending(plain_rt), "default execute left a pending job");
    JS_FreeContext(plain_ctx);
    JS_FreeRuntime(plain_rt);
    printf("PASS: default layout-out behavior without hooks\n");
}

void run_runtime_free_release() {
    HookLog log;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JS_SetContextOpaque(ctx, &log);
    JS_SetJobContextHooks(rt, &kHooks, &log);
    for (int i = 0; i < 3; ++i) {
        require(JS_EnqueueJob(ctx, noop_job, 0, nullptr) == 0,
                "enqueue failed");
    }
    require(log.captures == 3, "capture count != enqueue count");
    require(log.enters == 0 && log.leaves == 0,
            "unexecuted jobs must not enter/leave");
    require(log.releases == 0, "release before runtime free");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    require(log.releases == 3, "FreeRuntime did not release each job once");
    printf("PASS: FreeRuntime releases unexecuted jobs exactly once\n");
}

void run_execute_lifecycle() {
    TestRuntime t;
    JS_SetJobContextHooks(t.rt, &kHooks, &t.log);
    JobState state = { &t.log, reinterpret_cast<void *>(uintptr_t(1)) };
    enqueue_with_state(t.ctx, active_check_job, &state);
    JSContext *exec_ctx = nullptr;
    require(JS_ExecutePendingJob(t.rt, &exec_ctx) == 1, "execute failed");
    require(t.log.captures == 1 && t.log.enters == 1 && t.log.leaves == 1 &&
                t.log.releases == 1,
            "execute lifecycle: capture/enter/leave/release not all once");
    require(t.log.active == nullptr,
            "job context not restored to null after execution");
    printf("PASS: execute lifecycle retain/release once\n");
}

void run_exception_path() {
    TestRuntime t;
    JS_SetJobContextHooks(t.rt, &kHooks, &t.log);
    require(JS_EnqueueJob(t.ctx, throw_job, 0, nullptr) == 0,
            "enqueue failed");
    JSContext *exec_ctx = nullptr;
    require(JS_ExecutePendingJob(t.rt, &exec_ctx) == -1,
            "throwing job must return -1");
    JSValue exc = JS_GetException(t.ctx);
    JS_FreeValue(t.ctx, exc);
    require(t.log.enters == 1 && t.log.leaves == 1 && t.log.releases == 1,
            "exception path must enter/leave/release exactly once");
    printf("PASS: exception path enter/leave/release once\n");
}

void run_nested_drain() {
    TestRuntime t;
    JS_SetJobContextHooks(t.rt, &kHooks, &t.log);
    // The job queue is FIFO (enqueue: list_add_tail, execute: list head),
    // so A (the drainer) is enqueued first with tag 1 and runs first,
    // draining B (tag 2) before returning.
    JobState state_a = { &t.log, reinterpret_cast<void *>(uintptr_t(1)) };
    enqueue_with_state(t.ctx, nested_drain_job, &state_a);
    JobState state_b = { &t.log, reinterpret_cast<void *>(uintptr_t(2)) };
    enqueue_with_state(t.ctx, active_check_job, &state_b);
    JSContext *exec_ctx = nullptr;
    require(JS_ExecutePendingJob(t.rt, &exec_ctx) == 1, "execute A failed");
    require(!JS_IsJobPending(t.rt), "nested drain left a pending job");
    // captures at enqueue: A then B. execution: enter(A), enter(B) with
    // previous=A, leave(B) restoring A, release(B), leave(A) restoring null,
    // release(A).
    const std::vector<std::string> expected = {
        "enter:1", "enter:2", "leave:1", "release:2",
        "leave:0", "release:1",
    };
    require(t.log.sequence == expected,
            "nested drain hook sequence mismatch");
    printf("PASS: nested job context enter/leave restore\n");
}

void run_promise_reaction_capture() {
    TestRuntime t;
    JS_SetJobContextHooks(t.rt, &kHooks, &t.log);
    // Setup job runs with tag 1 and creates the .then() reaction there.
    require(JS_EnqueueJob(t.ctx, promise_setup_job, 0, nullptr) == 0,
            "promise setup enqueue failed");
    JSContext *exec_ctx = nullptr;
    require(JS_ExecutePendingJob(t.rt, &exec_ctx) == 1,
            "promise setup execute failed");
    // Settle job runs with tag 2; the reaction must NOT pick up this
    // context at settle.
    require(JS_EnqueueJob(t.ctx, promise_settle_job, 0, nullptr) == 0,
            "promise settle enqueue failed");
    require(JS_ExecutePendingJob(t.rt, &exec_ctx) == 1,
            "promise settle execute failed");
    require(JS_IsJobPending(t.rt), "reaction job not pending after settle");
    require(JS_ExecutePendingJob(t.rt, &exec_ctx) == 1,
            "reaction execute failed");
    require(!JS_IsJobPending(t.rt), "reaction execute left a pending job");
    // The reaction job must enter with the creation context (tag 1), not
    // the settle job's context (tag 2). The reject-side reaction of the
    // .then() is captured at creation too and released when the promise
    // fulfills without running.
    const std::vector<std::string> expected = {
        "enter:1", "leave:0", "release:1",   // setup job
        "enter:2", "release:1", "leave:0",
        "release:2",                         // settle job; the reject-side
                                             // reaction is freed (released)
                                             // inside the settle job
        "enter:1", "leave:0", "release:1",   // reaction job: creation context
    };
    require(t.log.sequence == expected,
            "promise reaction job context mismatch (creation vs settle)");
    int observed = 0;
    JSValue result = JS_Eval(t.ctx, kPromiseObservedScript,
                             sizeof(kPromiseObservedScript) - 1,
                             "<promise-observed>", JS_EVAL_TYPE_GLOBAL);
    require(JS_ToInt32(t.ctx, &observed, result) == 0,
            "cannot read promise observation");
    JS_FreeValue(t.ctx, result);
    require(observed == 1, "promise reaction callback did not run");
    printf("PASS: promise reactions run under the .then() creation context\n");
}

void run_capture_failure() {
    TestRuntime t;
    JS_SetJobContextHooks(t.rt, &kHooks, &t.log);
    t.log.fail_captures = 1;
    require(JS_EnqueueJob(t.ctx, noop_job, 0, nullptr) == -1,
            "capture failure must fail enqueue");
    require(!JS_IsJobPending(t.rt),
            "capture failure must not leave a pending job");
    require(t.log.captures == 1 && t.log.releases == 0,
            "failed capture must not release");
    // Runtime must stay usable after the failed enqueue.
    JSValue arg = JS_NewString(t.ctx, "x");
    require(JS_EnqueueJob(t.ctx, noop_job, 1, &arg) == 0,
            "enqueue after capture failure failed");
    JS_FreeValue(t.ctx, arg);  // caller owns its ref; enqueue dups internally
    require(JS_IsJobPending(t.rt), "no pending job after retry enqueue");
    JSContext *exec_ctx = nullptr;
    require(JS_ExecutePendingJob(t.rt, &exec_ctx) == 1,
            "execute after capture failure failed");
    printf("PASS: capture failure fails enqueue closed, runtime usable\n");
}

int origin_capture(JSContext *ctx, void *opaque, void **out) {
    HookLog *log = static_cast<HookLog *>(opaque);
    *out = nullptr;
    if (log->fail_origin_capture || log->fail_origin_after == 0) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    if (log->fail_origin_after > 0) --log->fail_origin_after;
    if (log->active) {++log->origin_captures; *out = log->active;}
    return 0;
}
void origin_release(void *, void *opaque) {
    ++static_cast<HookLog *>(opaque)->origin_releases;
}
bool origin_can_run(JSContext *, void *origin, void *opaque) {
    return static_cast<HookLog *>(opaque)->revoked_origins.count(origin) == 0;
}
bool origin_matches(JSContext *, void *origin, void *job, void *opaque) {
    return origin == (job ? job : static_cast<HookLog *>(opaque)->active);
}
const JSPromiseContextHooks kOriginHooks = {
    origin_capture, enter_hook, leave_hook, origin_release, origin_can_run, origin_matches,
};
JSValue observed_scope(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    HookLog *log = static_cast<HookLog *>(JS_GetContextOpaque(ctx));
    return JS_NewInt64(ctx, static_cast<int64_t>(reinterpret_cast<uintptr_t>(log->active)));
}
JSValue scope_eval(JSContext *ctx, const char *source) {
    JSValue result = JS_Eval(ctx, source, std::strlen(source), "promise-scope.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, error);
        std::string message = text ? text : "unknown Promise scope exception";
        JS_FreeCString(ctx, text); JS_FreeValue(ctx, error); fail(message);
    }
    return result;
}
void scope_script(TestRuntime &t, uintptr_t tag, const char *source) {
    t.log.active = reinterpret_cast<void *>(tag);
    JS_FreeValue(t.ctx, scope_eval(t.ctx, source));
}
void scope_expect(TestRuntime &t, const char *source) {
    JSValue result = scope_eval(t.ctx, source);
    require(JS_ToBool(t.ctx, result) == 1, source); JS_FreeValue(t.ctx, result);
}
void scope_drain(TestRuntime &t) {
    JSContext *ctx = nullptr;
    int status, count = 0;
    while ((status = JS_ExecutePendingJob(t.rt, &ctx)) > 0)
        require(++count < 100, "finite Promise scope microtask drain");
    require(status == 0, "Promise scope job exception");
}
void initialize_origins(TestRuntime &t) {
    JS_SetJobContextHooks(t.rt, &kHooks, &t.log);
    JS_SetPromiseContextHooks(t.rt, &kOriginHooks, &t.log);
    JSValue global = JS_GetGlobalObject(t.ctx);
    require(JS_SetPropertyStr(t.ctx, global, "scope", JS_NewCFunction(t.ctx, observed_scope, "scope", 0)) == 1,
            "install native scope observer");
    JS_FreeValue(t.ctx, global);
    scope_script(t, 0, "globalThis.observed = []");
}
const char kOriginPromise[] = "globalThis.p = new Promise(r => globalThis.resolve = r)";
const char kOriginFollow[] = "globalThis.follow = p.then(() => observed.push(scope()))";
void run_promise_scope_delivery() {
    TestRuntime t; initialize_origins(t);
    scope_script(t, 1, kOriginPromise); scope_script(t, 2, kOriginFollow);
    scope_script(t, 1, "resolve(42)"); scope_drain(t);
    scope_expect(t, "observed.length === 0");
    require(JS_IsDeferredPromiseJobPending(t.rt), "foreign following needs a loop turn");
    require(JS_IsJobPendingWithContext(t.rt, reinterpret_cast<void *>(uintptr_t(2))),
            "deferred delivery remains visible to its exact owning context");
    JS_PromoteDeferredPromiseJobs(t.rt); scope_drain(t);
    scope_expect(t, "observed.join() === '2'");
    scope_script(t, 1, kOriginPromise); scope_script(t, 1, kOriginFollow);
    scope_script(t, 2, "resolve(43)"); scope_drain(t);
    scope_expect(t, "observed.join() === '2'");
    JS_PromoteDeferredPromiseJobs(t.rt); scope_drain(t);
    scope_expect(t, "observed.join() === '2,1'");
    // An untagged module Promise inherits the resolving scope; ordinary
    // tagged Promises continue to use the following scope above.
    scope_script(t, 0, kOriginPromise); scope_script(t, 1, kOriginFollow);
    scope_script(t, 2, "resolve(44)"); scope_drain(t);
    scope_expect(t, "observed.join() === '2,1,2'");
    // The returned .then() Promise was created in scope 1 while this
    // untagged source ran its callback in scope 2. Its own completion is a
    // separate cross-scope turn; finish it before testing a revoked origin.
    require(JS_IsDeferredPromiseJobPending(t.rt), "module follower completion crosses back to its creation scope");
    JS_PromoteDeferredPromiseJobs(t.rt); scope_drain(t);
    require(!JS_IsDeferredPromiseJobPending(t.rt), "previous scenario leaves no deferred delivery");
    scope_script(t, 1, kOriginPromise); scope_script(t, 2, kOriginFollow);
    t.log.revoked_origins.insert(reinterpret_cast<void *>(uintptr_t(1)));
    scope_script(t, 2, "resolve(45)"); scope_drain(t);
    require(!JS_IsDeferredPromiseJobPending(t.rt), "revoked origin never queues delivery");
    scope_expect(t, "observed.join() === '2,1,2'");
    scope_script(t, 0, "delete globalThis.p; delete globalThis.follow; delete globalThis.resolve");
    JS_RunGC(t.rt);
    require(t.log.origin_captures == t.log.origin_releases, "GC releases every captured Promise origin");
    printf("PASS: Promise origin, following scope, event-loop deferral, revocation and GC\n");
}
void run_promise_scope_failures() {
    TestRuntime t; initialize_origins(t);
    t.log.active = reinterpret_cast<void *>(uintptr_t(1)); t.log.fail_origin_capture = true;
    JSValue functions[2];
    JSValue failed = JS_NewPromiseCapability(t.ctx, functions);
    require(JS_IsException(failed), "origin capture failure must fail Promise creation");
    JS_FreeValue(t.ctx, JS_GetException(t.ctx)); t.log.fail_origin_capture = false;
    scope_script(t, 1, kOriginPromise); scope_script(t, 2, kOriginFollow);
    JSValue global = JS_GetGlobalObject(t.ctx), resolve = JS_GetPropertyStr(t.ctx, global, "resolve");
    JS_FreeValue(t.ctx, global); t.log.active = reinterpret_cast<void *>(uintptr_t(1));
    const int releases = t.log.releases;
    JS_SetMemoryLimit(t.rt, 1); // existing values/functions need no allocations
    JSValue argument = JS_NewInt32(t.ctx, 7);
    JSValue result = JS_Call(t.ctx, resolve, JS_UNDEFINED, 1, &argument);
    JS_SetMemoryLimit(t.rt, static_cast<size_t>(-1));
    JS_FreeValue(t.ctx, result); JS_FreeValue(t.ctx, JS_GetException(t.ctx)); JS_FreeValue(t.ctx, resolve);
    require(t.log.releases == releases + 2,
            "failed delivery allocation releases both selected and unused reaction contexts");
    require(!JS_IsJobPending(t.rt) && !JS_IsDeferredPromiseJobPending(t.rt), "failed allocation queues no delivery");
    scope_script(t, 0, "delete globalThis.p; delete globalThis.follow; delete globalThis.resolve"); JS_RunGC(t.rt);
    require(t.log.origin_captures == t.log.origin_releases, "failed delivery leaks no Promise origins");
    printf("PASS: Promise capture and deferred allocation failures release ownership\n");
}
void run_deferred_runtime_free() {
    HookLog log;
    JSRuntime *rt = JS_NewRuntime(); JSContext *ctx = JS_NewContext(rt);
    JS_SetContextOpaque(ctx, &log); JS_SetJobContextHooks(rt, &kHooks, &log);
    JS_SetPromiseContextHooks(rt, &kOriginHooks, &log);
    log.active = reinterpret_cast<void *>(uintptr_t(1)); JS_FreeValue(ctx, scope_eval(ctx, kOriginPromise));
    log.active = reinterpret_cast<void *>(uintptr_t(2)); JS_FreeValue(ctx, scope_eval(ctx, "globalThis.follow = p.then(() => 1)"));
    log.active = reinterpret_cast<void *>(uintptr_t(1)); JS_FreeValue(ctx, scope_eval(ctx, "resolve(46)"));
    require(JS_IsDeferredPromiseJobPending(rt), "teardown test really has deferred delivery");
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    require(log.captures == log.releases, "runtime free releases every deferred job context");
    require(log.origin_captures == log.origin_releases, "runtime free releases every Promise origin");
    printf("PASS: runtime teardown releases unexecuted cross-context deliveries\n");
}


int mark_global_stream_wait(TestRuntime &t, const char *name = "p") {
    JSValue global = JS_GetGlobalObject(t.ctx);
    JSValue promise = JS_GetPropertyStr(t.ctx, global, name);
    JS_FreeValue(t.ctx, global);
    const int result = JS_MarkPromiseReactionsPassive(t.ctx, promise);
    JS_FreeValue(t.ctx, promise);
    return result;
}
void finish_observation_test(TestRuntime &t) {
    scope_script(t, 0, "for (const k of ['p','follow','resolve','task','normal','normalFollow','observed','thenGets','thenCalls']) delete globalThis[k]");
    JS_RunGC(t.rt);
    require(t.log.captures == t.log.releases, "observation control releases every active job capture");
    require(t.log.origin_captures == t.log.origin_releases, "observation control releases every inert origin capture");
}
void drain_observation_turns(TestRuntime &t) {
    scope_drain(t);
    for (int turn = 0; JS_IsDeferredPromiseJobPending(t.rt); ++turn) {
        require(turn < 8, "finite observation loop turns");
        JS_PromoteDeferredPromiseJobs(t.rt);
        scope_drain(t);
    }
}
void run_late_stream_observation() {
    TestRuntime t; initialize_origins(t);
    scope_script(t, 1, kOriginPromise);
    scope_script(t, 2, kOriginFollow);
    require(t.log.captures - t.log.releases == 2, "unmarked pending subscription starts with two active captures");
    t.log.active = reinterpret_cast<void *>(uintptr_t(3));
    require(mark_global_stream_wait(t) == 0, "late stream designation succeeds");
    require(t.log.active == reinterpret_cast<void *>(uintptr_t(3)), "late designation restores marking caller context");
    require(t.log.captures == t.log.releases, "late stream designation releases parked subscriber job leases");
    scope_script(t, 1, "resolve(47)"); scope_drain(t);
    scope_expect(t, "observed.length === 0");
    require(JS_IsDeferredPromiseJobPending(t.rt), "late designation preserves following-scope turn deferral");
    drain_observation_turns(t);
    scope_expect(t, "observed.join() === '2'");
    finish_observation_test(t);
    printf("PASS: late observation releases leases and preserves exact subscriber identity\n");
}
void run_async_prelude_observation() {
    TestRuntime t; initialize_origins(t);
    scope_script(t, 1, kOriginPromise);
    require(mark_global_stream_wait(t) == 0, "native stream wait designated before async use");
    scope_script(t, 1, "globalThis.task = (async () => { await Promise.resolve(); return await p; })()");
    scope_script(t, 2, "globalThis.follow = task.then(() => observed.push(scope()))");
    scope_script(t, 3, "0"); scope_drain(t);
    require(t.log.active == reinterpret_cast<void *>(uintptr_t(3)), "async conversion restores resolving caller context");
    require(t.log.captures == t.log.releases, "async prelude converts already published result subscriptions");
    scope_script(t, 1, "resolve(48)"); drain_observation_turns(t);
    scope_expect(t, "observed.join() === '2'");
    finish_observation_test(t);
    printf("PASS: async prelude retains following identity without a parked job lease\n");
}
void run_revoked_stream_observation() {
    TestRuntime t; initialize_origins(t);
    scope_script(t, 1, kOriginPromise); scope_script(t, 2, kOriginFollow);
    t.log.active = reinterpret_cast<void *>(uintptr_t(3));
    require(mark_global_stream_wait(t) == 0, "revocation setup late designation");
    t.log.revoked_origins.insert(reinterpret_cast<void *>(uintptr_t(2)));
    const int captured = t.log.captures;
    scope_script(t, 1, "resolve(49)"); drain_observation_turns(t);
    scope_expect(t, "observed.length === 0");
    require(t.log.captures == captured, "revoked subscriber receives no queued active job");
    finish_observation_test(t);
    printf("PASS: revoked late subscriber cannot acquire a delivery job\n");
}
void run_failed_stream_observation() {
    TestRuntime t; initialize_origins(t);
    scope_script(t, 1, kOriginPromise); scope_script(t, 2, kOriginFollow);
    t.log.active = reinterpret_cast<void *>(uintptr_t(3));
    const int released = t.log.releases;
    t.log.fail_origin_after = 1;
    require(mark_global_stream_wait(t) < 0, "partial inert capture failure fails designation closed");
    require(t.log.active == reinterpret_cast<void *>(uintptr_t(3)), "failed designation restores marking caller");
    require(t.log.releases == released + 1, "failed capture retains its still-active subscription");
    JS_FreeValue(t.ctx, JS_GetException(t.ctx));
    t.log.fail_origin_after = -1;
    require(mark_global_stream_wait(t) == 0, "designation can finish after capture failure");
    require(t.log.releases == released + 2, "retry converts remaining subscription exactly once");
    scope_script(t, 1, "resolve(50)"); drain_observation_turns(t);
    scope_expect(t, "observed.join() === '2'");
    finish_observation_test(t);
    printf("PASS: partial capture failure preserves identity, leases and retry cleanup\n");
}
void run_ordinary_promise_observation_control() {
    TestRuntime t; initialize_origins(t);
    scope_script(t, 1, kOriginPromise);
    require(mark_global_stream_wait(t) == 0, "only IO future explicitly designated");
    scope_script(t, 1, "globalThis.normal = new Promise(() => {}); globalThis.task = Promise.all([p, normal])");
    scope_script(t, 2, "globalThis.follow = task.then(() => observed.push(scope()))");
    scope_drain(t);
    require(t.log.active_job_captures[reinterpret_cast<void *>(uintptr_t(1))] == 2, "unrelated pending input retains both ordinary active captures");
    scope_script(t, 1, "resolve(51)"); drain_observation_turns(t);
    require(t.log.active_job_captures[reinterpret_cast<void *>(uintptr_t(1))] == 2, "IO completion does not erase unrelated pending input policy");
    scope_expect(t, "observed.length === 0");
    finish_observation_test(t);
    printf("PASS: mixed ordinary pending input keeps its active capture policy\n");
}
void run_native_forwarding_observation() {
    const char *expressions[] = {
        "globalThis.task = new Promise(r => r(p))",
        "globalThis.task = Promise.all([p])",
        "globalThis.task = Promise.all([Promise.resolve('ready'), p])",
        "globalThis.task = Promise.race([p])",
        "globalThis.task = Promise.any([p])",
        "globalThis.task = Promise.allSettled([p])",
        "globalThis.task = Promise.resolve().then(() => p)",
        "globalThis.task = (async () => p)()"
    };
    for (const char *expression : expressions) {
        TestRuntime t; initialize_origins(t);
        scope_script(t, 1, kOriginPromise);
        require(mark_global_stream_wait(t) == 0, "forwarding IO input designated");
        scope_script(t, 1, expression);
        scope_script(t, 2, "globalThis.follow = task.then(() => observed.push(scope()))");
        scope_script(t, 3, "0"); drain_observation_turns(t);
        require(t.log.captures == t.log.releases, "native forwarding leaves no parked active subscriber leases");
        require(t.log.active == reinterpret_cast<void *>(uintptr_t(3)), "forwarding restores original resolving caller");
        scope_script(t, 1, "resolve(52)"); drain_observation_turns(t);
        scope_expect(t, "observed.join() === '2'");
        finish_observation_test(t);
    }
    printf("PASS: eight native aggregate and adoption paths preserve following identity\n");
}
void run_overridden_then_observation_control() {
    const char *expressions[] = {
        "globalThis.task = new Promise(r => r(p))",
        "globalThis.task = Promise.all([p])"
    };
    for (const char *expression : expressions) {
        TestRuntime t; initialize_origins(t);
        scope_script(t, 1, kOriginPromise);
        require(mark_global_stream_wait(t) == 0, "overridden then source designated");
        scope_script(t, 1, "globalThis.thenGets=0; globalThis.thenCalls=0; Object.defineProperty(p,'then',{get(){ ++thenGets; return function(){ ++thenCalls; }; }})");
        scope_script(t, 1, expression);
        scope_script(t, 2, "globalThis.follow = task.then(() => observed.push(scope()))");
        drain_observation_turns(t);
        scope_expect(t, "thenGets === 1 && thenCalls === 1 && observed.length === 0");
        require(t.log.captures - t.log.releases == 2, "overridden then cannot designate an unforwarded ordinary result");
        finish_observation_test(t);
    }
    printf("PASS: overridden then is read once and cannot forge native forwarding\n");
}
int observation_interrupt(JSRuntime *, void *opaque) {
    ++*static_cast<int *>(opaque); return 1;
}
void run_observation_interrupt_control() {
    TestRuntime t; initialize_origins(t);
    scope_script(t, 1, kOriginPromise);
    scope_script(t, 2, "for(let i=0;i<8000;++i) p.then(() => 0)");
    require(t.log.captures - t.log.releases == 16000, "interrupt setup owns actual pending subscriptions");
    t.log.active = reinterpret_cast<void *>(uintptr_t(3));
    int interrupts = 0;
    JS_SetInterruptHandler(t.rt, observation_interrupt, &interrupts);
    require(mark_global_stream_wait(t) < 0 && interrupts > 0, "bulk observation conversion honors VM interruption");
    require(t.log.active == reinterpret_cast<void *>(uintptr_t(3)), "interrupted conversion restores marking caller");
    require(t.log.captures > t.log.releases, "interrupted conversion does not discard unconverted active captures");
    JS_SetInterruptHandler(t.rt, nullptr, nullptr);
    JS_FreeValue(t.ctx, JS_GetException(t.ctx));
    finish_observation_test(t);
    printf("PASS: bulk conversion respects the existing interrupt budget and releases partial state\n");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2) {
        const std::string name = argv[1];
        if (name == "late-observer") run_late_stream_observation();
        else if (name == "ordinary") run_ordinary_promise_observation_control();
        else if (name == "override") run_overridden_then_observation_control();
        else if (name == "interrupt") run_observation_interrupt_control();
        else fail("unknown observation selector");
        return 0;
    }
    require(argc == 1, "default observation unit or one exact selector");
    run_default_behavior();
    run_runtime_free_release();
    run_execute_lifecycle();
    run_exception_path();
    run_nested_drain();
    run_promise_reaction_capture();
    run_capture_failure();
    run_promise_scope_delivery();
    run_promise_scope_failures();
    run_deferred_runtime_free();
    run_late_stream_observation();
    run_async_prelude_observation();
    run_revoked_stream_observation();
    run_failed_stream_observation();
    run_ordinary_promise_observation_control();
    run_native_forwarding_observation();
    run_overridden_then_observation_control();
    run_observation_interrupt_control();
    printf("PASS: QuickJS job-context hook semantics\n");
    return 0;
}

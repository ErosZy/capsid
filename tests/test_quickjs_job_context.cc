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
        return 0;
    }
    *out_job_context =
        reinterpret_cast<void *>(static_cast<uintptr_t>(++log->next_tag));
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

}  // namespace

int main() {
    run_default_behavior();
    run_runtime_free_release();
    run_execute_lifecycle();
    run_exception_path();
    run_nested_drain();
    run_promise_reaction_capture();
    run_capture_failure();
    printf("PASS: QuickJS job-context hook semantics\n");
    return 0;
}

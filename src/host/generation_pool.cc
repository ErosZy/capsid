// GenerationPool implementation — see generation_pool.h. One pump thread
// owns every slot executor's event drain; a replacement spawn runs on its
// own thread so the pump never blocks on a spawn or on the process-global
// startup-permit queue (direction A: the queue is the Host-wired
// StartupPermitCoordinator, shared with the deploy path).

#include "host/generation_pool.h"

#include "host/managed_admin_backend.h"
#include "host/metrics.h"
#include "host/structured_log.h"
#include "host/worker_recovery.h"
#include "host/worker_capacity_ledger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <limits>
#include <utility>

namespace capsid::host {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::uint64_t kNoDue = 0;

std::uint64_t steady_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now().time_since_epoch())
            .count());
}

// M2 item 7 (design §12.2): single write path for every pool event.
// Null log (unit fixtures without the process-wide instance) is a no-op.
void emit_log(StructuredLog* log, LogLane lane, LogFields fields) {
    if (log != nullptr) {
        log->log(lane, std::move(fields));
    }
}

void count_event(MetricsRegistry* metrics,
                 const std::string& event,
                 const std::string& app,
                 const std::string& generation) {
    if (metrics != nullptr) {
        metrics->count_worker_event(event, app, generation);
    }
}

}  // namespace

GenerationPool::GenerationPool(GenerationPoolOptions options)
    : options_(std::move(options)), policy_(options_.recovery) {
    recovery_state_.application = options_.application_id;
    recovery_state_.generation = options_.generation_digest;
    lifecycle_.phase = ServiceLifecyclePhase::kActive;
    ActiveStateDocument document;
    document.state = ActiveServiceState::kActive;
    document.application = options_.application_id;
    document.version = options_.version;
    document.generation = options_.generation_digest;
    lifecycle_.document = document;
}

GenerationPool::~GenerationPool() {
    // Same bounded teardown as the facades: drain, then reap everything
    // (every executor thread joins; no detached thread is ever left).
    request_drain();
    std::string error;
    wait(&error);
}

std::shared_ptr<GenerationPool> GenerationPool::create(
    GenerationPoolOptions options, std::string* error) {
    if (options.workers == 0) {
        if (error != nullptr) {
            *error = "generation pool requires at least one worker";
        }
        return nullptr;
    }
    if (options.application_id.empty() || options.generation_digest.empty() ||
        options.version.empty()) {
        if (error != nullptr) {
            *error = "generation pool identity is incomplete";
        }
        return nullptr;
    }
    // The recovery controller validates the full identifier contract at the
    // first instability; this cheap gate mirrors the controller's policy
    // contract (worker_recovery.cc valid_policy) so a misconfigured pool
    // fails here with a clear message instead of silently serving N-1
    // forever (a zero field would make every decision an error).
    if (options.recovery.max_events == 0U ||
        options.recovery.window_ms == 0U ||
        options.recovery.backoff_initial_ms == 0U ||
        options.recovery.backoff_maximum_ms <
            options.recovery.backoff_initial_ms ||
        options.recovery.jitter_basis_points > 10000U ||
        options.recovery.stable_reset_ms == 0U ||
        options.recovery.replacements_concurrent_per_app == 0U) {
        if (error != nullptr) {
            *error = "invalid recovery policy (mirrors the controller's "
                     "contract: max_events/window_ms/backoff_initial_ms/"
                     "stable_reset_ms/replacements_concurrent_per_app "
                     "non-zero, backoff_maximum_ms >= backoff_initial_ms, "
                     "jitter_basis_points <= 10000)";
        }
        return nullptr;
    }
    if (!options.factory) {
        if (error != nullptr) {
            *error = "generation pool requires a worker factory";
        }
        return nullptr;
    }

    std::shared_ptr<GenerationPool> pool(new GenerationPool(std::move(options)));

    // N→READY barrier (§8.2): every slot's worker must be READY before the
    // pool is handed out. A partial failure reaps everything started.
    pool->slots_.reserve(pool->options_.workers);
    std::string spawn_error;
    for (std::uint32_t index = 0; index < pool->options_.workers; ++index) {
        std::shared_ptr<WorkerExecutor> executor(new WorkerExecutor());
        executor->set_channel_budget(pool->options_.channel_budget);
        executor->set_log_identity(pool->options_.application_id,
                                   pool->options_.generation_digest);
        // A temporary weak.lock() can become the last owner and invoke
        // the pool destructor on this executor thread, which then waits
        // for its own exit. The pool owns and joins the executor instead.
        executor->set_event_notifier([owner = pool.get()] {
            owner->notify_executor_event();
        });
        if (!executor->start(pool->options_.factory, &spawn_error)) {
            if (error != nullptr) {
                *error = "generation pool worker " +
                         std::to_string(index + 1) + "/" +
                         std::to_string(pool->options_.workers) +
                         " failed to start: " + spawn_error;
            }
            // Reap exactly the started slots (including this one's partial
            // state — a failed start leaves nothing behind by contract).
            for (const Slot& slot : pool->slots_) {
                slot.executor->stop_and_join();
            }
            // The pump never started, so nothing will ever drive this pool
            // to kDead; the destructor's drain+wait must not block forever
            // on it. Mark the pool dead up front — it never served.
            pool->state_ = State::kDead;
            return nullptr;
        }
        Slot slot;
        slot.executor = std::move(executor);
        slot.ready = true;
        pool->slots_.push_back(std::move(slot));
        if (pool->options_.on_worker_started) {
            pool->options_.on_worker_started(
                pool->slots_.back().executor.get());
        }
    }
    pool->replacement_due_ms_.assign(pool->slots_.size(), kNoDue);
    pool->state_ = State::kActive;
    pool->stable_since_ms_ = steady_ms();
    // The pump thread captures the BARE pointer, never a shared_ptr: a
    // self-held reference would keep the pool alive forever, so an
    // abandoned pool (e.g. an aborted §9.3 activation) could never be
    // destroyed — its drain would never run and every worker would leak.
    // Lifetime is safe: wait() joins the pump before the pool is freed,
    // and every pump exit runs inside finalize_drain.
    pool->pump_thread_ = std::thread([target = pool.get()] {
        target->pump_loop();
    });
    return pool;
}

std::shared_ptr<GenerationPool> GenerationPool::create_adopted(
    GenerationPoolOptions options, std::vector<capsid_worker*> warmed,
    std::string* error) {
    if (options.workers == 0 || warmed.size() != options.workers) {
        if (error != nullptr) {
            *error = "adopted generation pool requires exactly "
                     "options.workers pre-warmed workers";
        }
        // Mismatched input: destroy every worker handed in — the caller
        // transferred ownership by calling create_adopted.
        for (capsid_worker* worker : warmed) {
            capsid_worker_destroy(worker);
        }
        return nullptr;
    }
    if (options.application_id.empty() || options.generation_digest.empty() ||
        options.version.empty()) {
        if (error != nullptr) {
            *error = "generation pool identity is incomplete";
        }
        for (capsid_worker* worker : warmed) {
            capsid_worker_destroy(worker);
        }
        return nullptr;
    }
    // Same recovery-policy gate as create() (worker_recovery.cc valid_policy).
    if (options.recovery.max_events == 0U ||
        options.recovery.window_ms == 0U ||
        options.recovery.backoff_initial_ms == 0U ||
        options.recovery.backoff_maximum_ms <
            options.recovery.backoff_initial_ms ||
        options.recovery.jitter_basis_points > 10000U ||
        options.recovery.stable_reset_ms == 0U ||
        options.recovery.replacements_concurrent_per_app == 0U) {
        if (error != nullptr) {
            *error = "invalid recovery policy (mirrors the controller's "
                     "contract: max_events/window_ms/backoff_initial_ms/"
                     "stable_reset_ms/replacements_concurrent_per_app "
                     "non-zero, backoff_maximum_ms >= backoff_initial_ms, "
                     "jitter_basis_points <= 10000)";
        }
        for (capsid_worker* worker : warmed) {
            capsid_worker_destroy(worker);
        }
        return nullptr;
    }
    if (!options.factory) {
        if (error != nullptr) {
            *error = "generation pool requires a worker factory "
                     "(for replacements)";
        }
        for (capsid_worker* worker : warmed) {
            capsid_worker_destroy(worker);
        }
        return nullptr;
    }

    std::shared_ptr<GenerationPool> pool(new GenerationPool(std::move(options)));

    // No READY barrier: the adopter consumed it. Every slot starts READY.
    pool->slots_.reserve(pool->options_.workers);
    for (std::uint32_t index = 0; index < pool->options_.workers; ++index) {
        std::shared_ptr<WorkerExecutor> executor(new WorkerExecutor());
        executor->set_channel_budget(pool->options_.channel_budget);
        executor->set_log_identity(pool->options_.application_id,
                                   pool->options_.generation_digest);
        executor->set_event_notifier([owner = pool.get()] {
            owner->notify_executor_event();
        });
        if (!executor->adopt(warmed[index], error)) {
            // Ownership of the entire warmed fleet transferred at entry.
            // adopt() did not take this worker or the unvisited suffix;
            // destroy all of them. The adopted prefix has its own reapers.
            for (std::size_t remaining = index; remaining < warmed.size(); ++remaining) {
                capsid_worker_destroy(warmed[remaining]);
            }
            for (const Slot& slot : pool->slots_) {
                slot.executor->stop_and_join();
            }
            pool->state_ = State::kDead;  // the pump never started
            return nullptr;
        }
        Slot slot;
        slot.executor = std::move(executor);
        slot.ready = true;
        pool->slots_.push_back(std::move(slot));
        if (pool->options_.on_worker_started) {
            pool->options_.on_worker_started(
                pool->slots_.back().executor.get());
        }
    }
    pool->replacement_due_ms_.assign(pool->slots_.size(), kNoDue);
    pool->state_ = State::kActive;
    pool->stable_since_ms_ = steady_ms();
    // Bare pointer, same lifetime contract as create() (see above): never
    // self-hold the pool, or an aborted §9.3 activation leaks every worker.
    pool->pump_thread_ = std::thread([target = pool.get()] {
        target->pump_loop();
    });
    return pool;
}

void GenerationPool::request_drain() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ == State::kDead ||
            (state_ == State::kDraining && !quarantine_waiting_)) {
            return;  // idempotent
        }
        quarantine_waiting_ = false;
        state_ = State::kDraining;
        active_flag_.store(false, std::memory_order_relaxed);
        stable_since_ms_ = 0;
        drain_until_ = SteadyClock::now() + options_.drain_deadline;
        for (Slot& slot : slots_) {
            if (slot.executor->inflight() == 0 && slot.executor->connection_owners() == 0) {
                slot.shutdown_issued = true;
                slot.executor->request_shutdown();
            }
        }
    }
    // The pump re-runs its drain sweep; startup-permit waiters re-check
    // the pool's active flag (the abort predicate) and abandon.
    cv_.notify_all();
}

void GenerationPool::stop_and_join() {
    request_drain();
    std::string error;
    wait(&error);
}

bool GenerationPool::wait(std::string* error) {
    request_drain();  // wait() implies the drain request (idempotent)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return state_ == State::kDead; });
    }
    // Every thread finished its pool-mutex work before kDead was set
    // (finalize requires replacements_in_flight_ == 0 and every slot
    // exited); joining outside the mutex is safe and never self-deadlocks.
    std::call_once(wait_once_, [this] {
        if (pump_thread_.joinable()) {
            pump_thread_.join();
        }
        for (std::thread& thread : replacement_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        replacement_threads_.clear();
    });
    if (error != nullptr) {
        *error = "generation pool stopped";
    }
    return true;
}

std::int64_t GenerationPool::worker_process_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kActive) return 0;
    for (const Slot& slot : slots_) {
        if (slot.ready && slot.executor->available()) {
            return slot.executor->process_id();
        }
    }
    return 0;
}

WorkerExecutor* GenerationPool::pick_worker() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::kActive) {
        return nullptr;  // draining/dead: the caller answers 503
    }
    WorkerExecutor* best = nullptr;
    std::uint64_t best_load = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (!slot.ready) {
            continue;
        }
        WorkerExecutor* candidate = slot.executor.get();
        // §8.3: a dead worker is never routed to — ready is pool-maintained
        // (set false when its kExit is processed) and available() is the
        // executor's live exit state; both must hold.
        if (!candidate->available()) {
            continue;
        }
        // §8.2 load: inflight + pending client bytes (session-layer hook,
        // keyed by the executor identity; 0 when unset) + unhealthy penalty
        // (reserved for the session layer's notion of unhealthy — an exited
        // worker is excluded above, not penalized).
        std::uint64_t load = candidate->inflight() + candidate->channels();
        if (options_.client_bytes_loader) {
            load += options_.client_bytes_loader(candidate);
        }
        if (load < best_load) {
            best_load = load;
            best = candidate;
        }
    }
    return best;
}

WorkerExecutor* GenerationPool::current_worker() {
    return current_worker_snapshot().get();
}

std::shared_ptr<WorkerExecutor> GenerationPool::current_worker_snapshot() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::kActive) {
        return nullptr;
    }
    for (Slot& slot : slots_) {
        if (slot.ready && slot.executor->available()) {
            return slot.executor;
        }
    }
    return nullptr;
}

bool GenerationPool::recycle_worker(WorkerExecutor* target) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::kActive) {
        return false;
    }
    std::size_t slot = slots_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].executor.get() == target && slots_[index].ready) {
            slot = index;
            break;
        }
    }
    if (slot == slots_.size()) {
        return false;  // stale target: already removed or replaced
    }
    Slot& s = slots_[slot];
    // The worker is still ALIVE: the recycle must retire it. The
    // shutdown-issued marker makes its later EXIT an expected lifecycle
    // event, so handle_executor_exit never records it a second time (the
    // budget is charged exactly once, here). §8.3: the recycled worker
    // leaves the READY set immediately — the controller decides on the
    // capacity AFTER the removal, or a full fleet would never schedule a
    // replacement (1 >= 1).
    s.shutdown_issued = true;
    s.ready = false;
    const std::uint64_t now = steady_ms();
    std::uint32_t ready_after = 0;
    for (const Slot& other : slots_) {
        if (other.ready) {
            ++ready_after;
        }
    }
    WorkerInstabilityObservation observation;
    observation.kind = WorkerInstabilityKind::kHealthRecycle;
    observation.worker_generation = options_.generation_digest;
    observation.now_ms = now;
    observation.ready_workers_after_removal = ready_after;
    observation.target_ready_workers = options_.workers;
    observation.replacements_in_flight_for_app =
        static_cast<std::uint32_t>(replacements_scheduled_);
    observation.chosen_jitter_basis_points = 0;  // deterministic v1
    count_event(options_.metrics, "unhealthy", options_.application_id,
                options_.generation_digest);
    record_instability(slot, observation, "unhealthy");
    // record_instability armed the replacement (or began quarantine,
    // which requests every slot's shutdown); the target's shutdown is
    // issued regardless so the alive-but-unhealthy worker exits to the
    // reaper (the executor's worker thread owns the destroy).
    if (!quarantine_waiting_) s.executor->request_shutdown();
    return true;
}

void GenerationPool::record_instability(
    std::size_t slot, WorkerInstabilityObservation observation,
    const char* reason) {
    // Under the pool mutex.
    const WorkerRecoveryDecision decision = record_worker_instability(
        recovery_state_, policy_, lifecycle_, observation);
    if (!decision.ok) {
        // A malformed decision is an operator-facing failure, not a silent
        // one: keep serving at N-1 (or 0) without a replacement.
        std::fprintf(stderr,
                     "capsid-host: generation pool %s: recovery controller "
                     "rejected the instability record (%s)\n",
                     options_.application_id.c_str(),
                     decision.error.message.c_str());
        return;
    }
    recovery_state_ = decision.state;
    // §12.1 recovery family: the remaining instability budget after this
    // counted event, and the backoff the decision chose.
    if (options_.metrics != nullptr) {
        const std::uint64_t remaining =
            decision.events_in_window >= policy_.max_events
                ? 0
                : static_cast<std::uint64_t>(
                      policy_.max_events - decision.events_in_window);
        options_.metrics->set_recovery_instability_budget_remaining(
            options_.application_id, remaining);
        options_.metrics->set_recovery_backoff_ms(
            options_.application_id, decision.replacement_delay_ms);
    }
    if (decision.begin_quarantine) {
        begin_quarantine();
        return;
    }
    if (decision.schedule_replacement) {
        Slot& s = slots_[slot];
        s.replacement_in_flight = true;
        ++replacements_scheduled_;
        s.replacement_reason = reason;
        replacement_due_ms_[slot] =
            observation.now_ms + decision.replacement_delay_ms;
        cv_.notify_all();  // the pump re-evaluates its deadline wait
    }
}

void GenerationPool::begin_quarantine() {
    // Under the pool mutex.
    if (state_ != State::kActive) return;
    LogFields fields;
    fields.level = "error";
    fields.event = log_events::kQuarantine;
    fields.app = options_.application_id;
    fields.generation = options_.generation_digest;
    fields.result = "crash_budget_exceeded";
    fields.message = "quarantined: crash budget exceeded";
    emit_log(options_.log, LogLane::kControl, std::move(fields));
    if (options_.metrics != nullptr) {
        options_.metrics->count_recovery_quarantine(options_.application_id);
    }
    // Stop routing/replacement immediately. Dispatch persistence from the
    // pump without mutex_: App operations take that mutex in the opposite
    // order when committing or aborting an activation.
    quarantine_waiting_ = true;
    quarantine_notification_pending_ = true;
    state_ = State::kDraining;
    active_flag_.store(false, std::memory_order_relaxed);
    stable_since_ms_ = 0;
    cv_.notify_all();
}

void GenerationPool::finish_quarantine_locked() {
    quarantine_waiting_ = false;
    for (Slot& slot : slots_) {
        slot.shutdown_issued = true;
        slot.executor->request_shutdown();
    }
    cv_.notify_all();
}

void GenerationPool::set_event_sink(
    std::function<void(const WorkerExecutor*, WorkerEvent)> sink) {
    // The pump reads options_.event_sink under the same mutex, so the swap
    // is atomic with respect to every drain sweep.
    std::lock_guard<std::mutex> lock(mutex_);
    options_.event_sink = std::move(sink);
}

std::uint64_t GenerationPool::inflight() const {
    std::unique_lock<std::mutex> lock(mutex_);
    std::uint64_t total = 0;
    for (const Slot& slot : slots_) {
        total += slot.executor->inflight();
    }
    for (const std::shared_ptr<WorkerExecutor>& executor : retired_) {
        total += executor->inflight();
    }
    return total;
}

std::uint32_t GenerationPool::ready_workers() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_workers_locked();
}

std::uint32_t GenerationPool::ready_workers_locked() const {
    std::uint32_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.ready) {
            ++count;
        }
    }
    return count;
}

GenerationPool::State GenerationPool::state() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return state_;
}

std::string_view GenerationPool::application_id() const {
    return options_.application_id;
}

std::string_view GenerationPool::version() const {
    return options_.version;
}

std::string_view GenerationPool::generation_digest() const {
    return options_.generation_digest;
}

void GenerationPool::notify_executor_event() {
    if (options_.on_executor_notify) options_.on_executor_notify();
    std::lock_guard<std::mutex> lock(mutex_);
    events_pending_ = true;
    cv_.notify_all();
}

void GenerationPool::pump_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (quarantine_notification_pending_) {
            quarantine_notification_pending_ = false;
            lock.unlock();
            if (options_.on_quarantine) options_.on_quarantine();
            lock.lock();
        }
        if (quarantine_waiting_ &&
            (!options_.quarantine_completion || options_.quarantine_completion->load())) {
            finish_quarantine_locked();
        }
        // Drain sweep: re-issue shutdowns for slots swapped in after
        // request_drain() (a replacement READY while draining must also be
        // shut down), and complete the drain when the fleet is empty.
        if (state_ == State::kDraining && !quarantine_waiting_) {
            for (Slot& slot : slots_) {
                if (!slot.executor->exited() &&
                    ((slot.executor->inflight()==0 && slot.executor->connection_owners()==0) ||
                     (drain_until_ && SteadyClock::now()>=*drain_until_))) {
                    slot.executor->request_shutdown();
                }
            }
            if (fleet_exited() && replacements_in_flight_ == 0) {
                // finalize_drain takes the mutex itself; release first.
                lock.unlock();
                finalize_drain();
                return;
            }
        }
        // Continuous stability resets the replacement backoff (v1: the
        // pool-wide base — the fleet's last full-READY epoch).
        if (stable_since_ms_ != 0 && policy_.stable_reset_ms != 0) {
            const std::uint64_t now = steady_ms();
            if (now - stable_since_ms_ >= policy_.stable_reset_ms) {
                const WorkerStabilityResult stability = observe_worker_stability(
                    recovery_state_, policy_, stable_since_ms_, now);
                if (stability.ok) {
                    recovery_state_ = stability.state;
                }
                stable_since_ms_ = 0;
            }
        }
        // Launch due replacements (§8.3): a scheduled slot spawns at its
        // backoff deadline; the spawn runs on its own thread so the pump
        // never blocks on the semaphore or the factory.
        if (state_ == State::kActive) {
            const std::uint64_t now = steady_ms();
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                if (replacement_due_ms_[index] != kNoDue &&
                    replacement_due_ms_[index] <= now) {
                    replacement_due_ms_[index] = kNoDue;
                    ++replacements_in_flight_;
                    replacement_threads_.emplace_back(
                        [owner = this, index] {
                            // wait() joins replacement producers too;
                            // they must not release the last pool owner
                            // on a thread that its destructor would join.
                            owner->run_replacement(index);
                        });
                }
            }
        }
        // Wait for work: an event notification, a replacement deadline, or
        // the drain completing.
        std::uint64_t next_due = std::numeric_limits<std::uint64_t>::max();
        for (const std::uint64_t due : replacement_due_ms_) {
            if (due != kNoDue && due < next_due) {
                next_due = due;
            }
        }
        if (!events_pending_ &&
            !(state_ == State::kDraining && !quarantine_waiting_ && fleet_exited() &&
              replacements_in_flight_ == 0)) {
            // request_drain changes state without adding an event. Wake
            // an ACTIVE wait on that transition even when live channels
            // still pin the fleet, so the next iteration can enforce the
            // drain deadline. Waiting for fleet_exited here would leave a
            // quiet WebSocket generation asleep indefinitely.
            //
            // The no-deadline wait's predicate additionally covers "a
            // replacement was scheduled" (any due now present): a schedule
            // notify can arrive from another thread — the spawn thread's
            // run_replacement failure path — when no deadline was being
            // waited on, and without the due_pending clause that notify
            // would find the predicate false and the wait would sleep
            // through the new deadline forever (a timeout-less predicate
            // wait re-sleeps instead of returning). With the clause, the
            // wait returns, the loop recomputes next_due, and the
            // deadline wait below sleeps until the replacement is due.
            const auto wake = [this] {
                return events_pending_ ||
                       due_pending_locked() ||
                       state_ == State::kDraining;
            };
            if (state_ == State::kDraining) {
                // A host ACK can return the final channel slot without a
                // response event. Recheck liveness and the absolute bound.
                cv_.wait_for(lock, std::chrono::milliseconds(50));
            } else if (next_due == std::numeric_limits<std::uint64_t>::max()) {
                cv_.wait(lock, wake);
            } else {
                // The deadline is the timeout: expiry returns even when the
                // predicate is false, so no due_pending clause is needed
                // here (and none may be — a permanently-true predicate
                // would busy-loop the wait_until).
                cv_.wait_until(
                    lock,
                    SteadyClock::time_point(
                        std::chrono::duration_cast<SteadyClock::duration>(
                            std::chrono::milliseconds(next_due))),
                    [this] {
                        return events_pending_ ||
                               state_ == State::kDraining;
                    });
            }
        }
        events_pending_ = false;
        // Drain every slot's event queue (this is the owner thread for all
        // of them) and handle exits. Non-kExit events are forwarded to the
        // §9.2 event sink (the listener's sessions); kExit stays pool-owned
        // for the replacement machinery but is forwarded too, so the
        // listener can fail the requests pinned to a dead worker (the
        // executor emits no per-request failure for a worker exit).
        std::vector<std::size_t> exited;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            std::deque<WorkerEvent> batch =
                slots_[index].executor->drain_events();
            for (const WorkerEvent& event : batch) {
                if (event.type == WorkerEvent::Type::kExit) {
                    exited.push_back(index);
                }
                if (options_.event_sink) {
                    options_.event_sink(slots_[index].executor.get(), event);
                }
            }
        }
        // WP-05 §9.2: retired executors (replaced slots with pinned
        // requests) are drained here too — a pinned request's response
        // events must reach the session layer even after its slot was
        // replaced, or the request hangs forever. Nothing to bookkeep: a
        // retired executor is never replaced again.
        for (const std::shared_ptr<WorkerExecutor>& retired : retired_) {
            std::deque<WorkerEvent> batch = retired->drain_events();
            for (const WorkerEvent& event : batch) {
                if (options_.event_sink) {
                    options_.event_sink(retired.get(), event);
                }
            }
        }
        for (const std::size_t index : exited) {
            handle_executor_exit(index);
        }
    }
}

bool GenerationPool::fleet_exited() const {
    for (const Slot& slot : slots_) {
        if (!slot.executor->exited()) {
            return false;
        }
    }
    for (const auto& executor : retired_) {
        if (!executor->exited()) return false;
    }
    return true;
}

// Any replacement scheduled (a due present), regardless of whether its
// deadline has elapsed. Called from the pump's wait predicate under the
// pool mutex: it turns a schedule notify into a wake even when the pump
// was sleeping on the no-deadline wait — the wait returns, the deadline
// is recomputed, and the pump sleeps until the replacement is due.
bool GenerationPool::due_pending_locked() const {
    for (const std::uint64_t due : replacement_due_ms_) {
        if (due != kNoDue) {
            return true;
        }
    }
    return false;
}

void GenerationPool::handle_executor_exit(std::size_t slot) {
    // Under the pool mutex (pump thread).
    Slot& s = slots_[slot];
    if (!s.ready) {
        // Already removed (a repeated exit bookkeeping pass); count nothing
        // twice.
        return;
    }
    s.ready = false;  // §8.3: EXIT removes the slot from the READY set now
    stable_since_ms_ = 0;
    if (options_.on_worker_exited) {
        options_.on_worker_exited(s.executor.get());
    }
    // A shutdown the pool itself requested is an expected lifecycle event:
    // the health recycle already recorded its instability and scheduled
    // the replacement, and the drain wants nothing — never recorded again
    // (the budget is charged exactly once, by the single recovery engine).
    if (s.shutdown_issued) {
        return;
    }
    const std::uint64_t now = steady_ms();
    std::uint32_t ready_after = 0;
    for (const Slot& other : slots_) {
        if (other.ready) {
            ++ready_after;
        }
    }
    WorkerInstabilityObservation observation;
    observation.kind = WorkerInstabilityKind::kUnexpectedExit;
    observation.worker_generation = options_.generation_digest;
    observation.now_ms = now;
    observation.ready_workers_after_removal = ready_after;
    observation.target_ready_workers = options_.workers;
    observation.replacements_in_flight_for_app =
        static_cast<std::uint32_t>(replacements_scheduled_);
    observation.chosen_jitter_basis_points = 0;  // deterministic v1
    count_event(options_.metrics, "crash", options_.application_id,
                options_.generation_digest);
    record_instability(slot, observation, "crashed");
}

void GenerationPool::run_replacement(std::size_t slot) {
    // Spawn thread. Never runs on the pump.
    auto abandon = [this, slot] {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            Slot& s = slots_[slot];
            s.replacement_in_flight = false;
            if (replacements_scheduled_ > 0) {
                --replacements_scheduled_;
            }
            --replacements_in_flight_;
            cv_.notify_all();
        }
    };

    // §8.3: a generation that is no longer active never starts a
    // replacement.
    if (!active_flag_.load(std::memory_order_relaxed)) {
        abandon();
        return;
    }

    // Direction A: the replacement joins the process-global fair
    // startup-permit queue (design §10.5.6) and holds its grant across
    // the respawn, so a crash-looping App cannot persistently queue ahead
    // of another App's deploy. The abort predicate abandons the wait when
    // this pool's drain begins, so the pump's replacements_in_flight_ == 0
    // wait can never deadlock behind a permit nobody will hand out.
    bool grant_held = false;
    if (options_.startup_permits != nullptr) {
        StartupPermitRequest request;
        request.application = options_.application_id;
        request.generation = options_.generation_digest;
        request.lane = StartupPermitLane::kReplacement;
        const auto aborted = [this] {
            return !active_flag_.load(std::memory_order_relaxed);
        };
        if (!options_.startup_permits->enqueue_and_wait(request, aborted)) {
            // Queue full, singleflight, stop, or the drain aborted the
            // wait: nothing was granted; the pool-side bookkeeping
            // releases so the slot can be re-scheduled by the next exit
            // event (or the drain completes — the wait() join reclaims
            // this thread).
            abandon();
            return;
        }
        grant_held = true;
        if (options_.metrics != nullptr) {
            options_.metrics->count_recovery_startup_permit_grant(
                options_.application_id);
        }
    }

    // Spawn and READY through the same factory (same artifact, same
    // effective config — §8.3) into a fresh executor.
    std::shared_ptr<WorkerExecutor> replacement(new WorkerExecutor());
    replacement->set_channel_budget(options_.channel_budget);
    replacement->set_log_identity(options_.application_id,
                                  options_.generation_digest);
    replacement->set_event_notifier([this] { notify_executor_event(); });
    std::string spawn_error;
    const bool ready = replacement->start(options_.factory, &spawn_error);
    if (grant_held) {
        // The permit was consumed by this spawn/READY window; hand it to
        // the next waiter regardless of the outcome.
        options_.startup_permits->release_grant();
    }

    if (!ready) {
        // A failed replacement is an instability: record it (the crash
        // budget applies) and re-schedule with the next backoff step.
        // The singleflight bookkeeping releases FIRST: the failure
        // consumed the in-flight attempt, so the controller must see
        // replacements_scheduled_ at its pre-attempt value or the next
        // record trips the per-App singleflight check (1 >= 1) and the
        // retry dies silently.
        std::fprintf(stderr,
                     "capsid-host: generation pool %s: replacement spawn "
                     "failed: %s\n",
                     options_.application_id.c_str(), spawn_error.c_str());
        abandon();
        std::unique_lock<std::mutex> lock(mutex_);
        WorkerInstabilityObservation observation;
        observation.kind = WorkerInstabilityKind::kReplacementStartupFailure;
        observation.worker_generation = options_.generation_digest;
        observation.now_ms = steady_ms();
        observation.ready_workers_after_removal = ready_workers_locked();
        observation.target_ready_workers = options_.workers;
        observation.replacements_in_flight_for_app =
            static_cast<std::uint32_t>(replacements_scheduled_);
        observation.chosen_jitter_basis_points = 0;
        count_event(options_.metrics, "crash", options_.application_id,
                    options_.generation_digest);
        // A failed spawn is never installed, so the reason string is
        // meaningless here; it only names a later install.
        record_instability(slot, observation, "crashed");
        cv_.notify_all();
        return;
    }

    install_replacement(slot, std::move(replacement));
}

void GenerationPool::install_replacement(
    std::size_t slot, std::shared_ptr<WorkerExecutor> replacement) {
    WorkerExecutor* installed = replacement.get();
    std::string reason;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        Slot& s = slots_[slot];
        // Retire, never drop: a request pinned to the old generation slot
        // (a WorkerExecutor* handed out by pick_worker) must never touch
        // freed memory (§8.2 pinning). The retired executor is reaped at
        // finalize_drain, outside the mutex, where its destructor's join
        // cannot deadlock against the event notifier.
        retired_.push_back(std::move(s.executor));
        s.executor = std::move(replacement);
        s.ready = true;  // the fleet is N again (§8.3)
        // A replacement installed into a draining pool is shut down right
        // here. The pump's drain sweep would issue the shutdown — but
        // nothing will wake the pump for it: this executor's READY events
        // never reach the pool notifier, and fleet_exited() stays false
        // until the worker dies, so the pump's wait predicate never turns
        // true. Shutting down now lets the worker's kExit wake the pump
        // and complete the drain.
        if (state_ == State::kDraining && !quarantine_waiting_) {
            s.executor->request_shutdown();
        }
        s.replacement_in_flight = false;
        if (replacements_scheduled_ > 0) {
            --replacements_scheduled_;
        }
        reason = std::move(s.replacement_reason);
        s.replacement_reason.clear();
        if (ready_workers_locked() == options_.workers) {
            stable_since_ms_ = steady_ms();  // full-READY epoch base
        }
        // Singleflight gap: another slot may have died while this
        // replacement was in flight, its exit decision suppressed by the
        // per-App singleflight gate. Re-check the capacity now that the
        // singleflight cleared — every dead slot without a covered
        // replacement gets one immediately.
        if (state_ == State::kActive) {
            for (std::size_t index = 0; index < slots_.size(); ++index) {
                Slot& other = slots_[index];
                if (!other.ready && !other.replacement_in_flight) {
                    other.replacement_in_flight = true;
                    ++replacements_scheduled_;
                    replacement_due_ms_[index] = steady_ms();
                }
            }
        }
        cv_.notify_all();
    }
    // §12.2: a replacement publish is a process-lifecycle event (control
    // lane, never dropped). The message distinguishes the instability
    // kind that triggered the replacement (direction A: the pool is the
    // only recovery engine, so the reason is pool-owned).
    LogFields fields;
    fields.event = log_events::kWorkerReplaced;
    fields.app = options_.application_id;
    fields.generation = options_.generation_digest;
    fields.result = "replaced";
    fields.message = (reason == "unhealthy")
                         ? "replaced unhealthy worker"
                         : "replaced crashed worker";
    emit_log(options_.log, LogLane::kControl, std::move(fields));
    count_event(options_.metrics, "replacement", options_.application_id,
                options_.generation_digest);
    if (options_.on_worker_started) {
        options_.on_worker_started(installed);
    }
    // The observer runs outside mutex_, but is still a replacement
    // producer. Keep finalization from releasing its executor (and from
    // calling on_drain_complete) until the observer has returned.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --replacements_in_flight_;
        cv_.notify_all();
    }
}

void GenerationPool::finalize_drain() {
    // Under the pool mutex (pump thread).
    std::vector<std::shared_ptr<WorkerExecutor>> reaped;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        state_ = State::kDead;
        for (Slot& slot : slots_) {
            reaped.push_back(std::move(slot.executor));
        }
        slots_.clear();
        for (std::shared_ptr<WorkerExecutor>& executor : retired_) {
            reaped.push_back(std::move(executor));
        }
        retired_.clear();
        cv_.notify_all();
    }
    // External snapshots can keep an executor alive after its slot is
    // cleared. Its destructor is therefore no longer a join barrier. The
    // last notifier still borrows this pool, so explicitly join every
    // producer before publishing completion or allowing wait() to return.
    // The pool mutex is released: a final notifier may need that mutex.
    for (const auto& executor : reaped) {
        executor->stop_and_join();
    }
    // §9.4: the reaper finished — the drained generation's capacity count
    // releases now. Outside the pool mutex (the callback must be able to
    // take its own locks), after every executor thread has joined.
    if (options_.capacity_lease) options_.capacity_lease->reaped();
    // A child disappearing from /proc can precede this point: the sole
    // reaper still has executor bookkeeping to publish. Expose the actual
    // generation completion after its capacity category has been released.
    count_event(options_.metrics, "generation_drained", options_.application_id,
                options_.generation_digest);
    if (options_.on_drain_complete) {
        options_.on_drain_complete();
    }
    // Release the pool's last executor owners outside the mutex. External
    // snapshots may outlive these, but no executor producer remains live.
}

}  // namespace capsid::host

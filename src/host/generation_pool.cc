// GenerationPool implementation — see generation_pool.h. One pump thread
// owns every slot executor's event drain; a replacement spawn runs on its
// own thread so the pump never blocks on a spawn or on the process-global
// startup semaphore.

#include "host/generation_pool.h"

#include "host/worker_recovery.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <limits>
#include <set>
#include <utility>

namespace capsid::host {

namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr std::uint64_t kNoDue = 0;
constexpr std::size_t kMaxQueuedStartupPermits = 16;
constexpr std::size_t kDefaultStartupPermitLimit = 4;

std::uint64_t steady_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            SteadyClock::now().time_since_epoch())
            .count());
}

}  // namespace

// §8.3 global startup semaphore: a process-wide cap on concurrent worker
// spawns (deploys and replacements share the lane; WP-05 enqueues deploys
// through the same queue). Permits are granted under the mutex; a grant
// records the granted ticket so the waiting spawn thread can prove it was
// the grantee (the pure FairStartupPermitQueue API removes the granted
// request from the queue, so queue membership is not proof).
struct GenerationPool::StartupSemaphore {
    std::mutex mutex;
    std::condition_variable cv;
    FairStartupPermitQueue queue;
    std::size_t limit = kDefaultStartupPermitLimit;
    std::size_t in_flight = 0;
    std::set<std::uint64_t> granted_tickets;
};

namespace {
// Ticket counter shared by every pool so tickets stay unique queue-wide
// (the queue rejects duplicates).
std::atomic<std::uint64_t> g_next_startup_ticket{1};
}  // namespace

// One semaphore per process; every GenerationPool and (later) every
// Managed deploy shares it. The static member is the only entry into the
// private nested type.
GenerationPool::StartupSemaphore& GenerationPool::startup_semaphore() {
    static StartupSemaphore semaphore;
    return semaphore;
}

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
        // The notifier wakes the pump; weak capture so the notifier never
        // keeps a stopped pool alive (the executor would otherwise hold a
        // shared_ptr cycle back into the pool).
        executor->set_event_notifier([weak = pool->weak_from_this()] {
            if (std::shared_ptr<GenerationPool> p = weak.lock()) {
                {
                    std::lock_guard<std::mutex> lock(p->mutex_);
                    p->events_pending_ = true;
                }
                p->cv_.notify_all();
            }
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
    }
    pool->replacement_due_ms_.assign(pool->slots_.size(), kNoDue);
    pool->state_ = State::kActive;
    pool->stable_since_ms_ = steady_ms();
    pool->pump_thread_ = std::thread([pool] { pool->pump_loop(); });
    return pool;
}

void GenerationPool::request_drain() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ == State::kDraining || state_ == State::kDead) {
            return;  // idempotent
        }
        state_ = State::kDraining;
        active_flag_.store(false, std::memory_order_relaxed);
        stable_since_ms_ = 0;
        for (Slot& slot : slots_) {
            slot.shutdown_issued = true;
            slot.executor->request_shutdown();
        }
    }
    // The pump re-runs its drain sweep; permit waiters re-check the pool's
    // active flag and abandon.
    cv_.notify_all();
    startup_semaphore().cv.notify_all();
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
        // §8.2 load: inflight + pending client bytes (session-layer hook;
        // 0 when unset) + unhealthy penalty (reserved for the session
        // layer's notion of unhealthy — an exited worker is excluded
        // above, not penalized).
        std::uint64_t load = candidate->inflight();
        if (options_.client_bytes_loader) {
            load += options_.client_bytes_loader(index);
        }
        if (load < best_load) {
            best_load = load;
            best = candidate;
        }
    }
    return best;
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

void GenerationPool::pump_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        // Drain sweep: re-issue shutdowns for slots swapped in after
        // request_drain() (a replacement READY while draining must also be
        // shut down), and complete the drain when the fleet is empty.
        if (state_ == State::kDraining) {
            for (Slot& slot : slots_) {
                if (!slot.executor->exited()) {
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
                        [pool = shared_from_this(), index] {
                            pool->run_replacement(index);
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
            !(state_ == State::kDraining && fleet_exited() &&
              replacements_in_flight_ == 0)) {
            // The predicate must also cover drain completion: request_drain
            // and the spawn threads notify cv_ without setting
            // events_pending_ (no new events), and the predicate is what
            // turns those notifies into a re-check instead of a lost wake.
            // It must read the LIVE state — a stale snapshot captured
            // before the wait would sleep through a completion notify.
            const auto wake = [this] {
                return events_pending_ ||
                       (state_ == State::kDraining && fleet_exited() &&
                        replacements_in_flight_ == 0);
            };
            if (next_due == std::numeric_limits<std::uint64_t>::max()) {
                cv_.wait(lock, wake);
            } else {
                cv_.wait_until(
                    lock,
                    SteadyClock::time_point(
                        SteadyClock::duration(
                            static_cast<SteadyClock::duration::rep>(
                                next_due))),
                    wake);
            }
        }
        events_pending_ = false;
        // Drain every slot's event queue (this is the owner thread for all
        // of them) and handle exits.
        std::vector<std::size_t> exited;
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            std::deque<WorkerEvent> batch =
                slots_[index].executor->drain_events();
            for (const WorkerEvent& event : batch) {
                if (event.type == WorkerEvent::Type::kExit) {
                    exited.push_back(index);
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
    return true;
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
    const std::uint64_t now = steady_ms();
    std::uint32_t ready_after = 0;
    for (const Slot& other : slots_) {
        if (other.ready) {
            ++ready_after;
        }
    }
    WorkerInstabilityObservation observation;
    // A shutdown the pool itself requested is an expected lifecycle event
    // (no budget, no replacement); anything else is an unexpected exit.
    observation.kind = s.shutdown_issued
                           ? WorkerInstabilityKind::kNormalDrain
                           : WorkerInstabilityKind::kUnexpectedExit;
    observation.worker_generation = options_.generation_digest;
    observation.now_ms = now;
    observation.ready_workers_after_removal = ready_after;
    observation.target_ready_workers = options_.workers;
    observation.replacements_in_flight_for_app =
        static_cast<std::uint32_t>(replacements_scheduled_);
    observation.chosen_jitter_basis_points = 0;  // deterministic v1

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
    if (decision.begin_quarantine) {
        // Crash budget exhausted (§8.3): no replacement; the pool serves
        // at reduced capacity until the operator intervenes.
        std::fprintf(stderr,
                     "capsid-host: generation pool %s: crash budget "
                     "exhausted, replacement suppressed (quarantine)\n",
                     options_.application_id.c_str());
        return;
    }
    if (decision.schedule_replacement) {
        s.replacement_in_flight = true;
        ++replacements_scheduled_;
        replacement_due_ms_[slot] = now + decision.replacement_delay_ms;
        cv_.notify_all();  // the pump re-evaluates its deadline wait
    }
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
        startup_semaphore().cv.notify_all();
    };

    // §8.3: a generation that is no longer active never starts a
    // replacement.
    if (!active_flag_.load(std::memory_order_relaxed)) {
        abandon();
        return;
    }

    StartupSemaphore& sem = startup_semaphore();
    const std::uint64_t ticket =
        g_next_startup_ticket.fetch_add(1, std::memory_order_relaxed);
    StartupPermitRequest request;
    request.ticket = ticket;
    request.application = options_.application_id;
    request.generation = options_.generation_digest;
    request.lane = StartupPermitLane::kReplacement;
    {
        std::unique_lock<std::mutex> lock(sem.mutex);
        const StartupPermitQueueResult queued = enqueue_startup_permit_request(
            sem.queue, request, kMaxQueuedStartupPermits);
        sem.queue = queued.queue;
        if (!queued.ok) {
            // Queue full or invalid: nothing was granted; the pool-side
            // bookkeeping releases so the slot can be re-scheduled by the
            // next exit event.
            lock.unlock();
            abandon();
            return;
        }
        if (queued.joined_existing) {
            // Singleflight at the semaphore (the recovery controller's
            // per-App gate already prevents this for one pool; the queue
            // enforces it process-wide).
            lock.unlock();
            abandon();
            return;
        }
        // A permit may already be free (in_flight < limit): grant this
        // request now. Without this, the FIRST replacement of every
        // generation waits forever — grant_next_startup_permit is only
        // called by a *previous* spawn's release, and there is none.
        if (sem.in_flight < sem.limit) {
            const StartupPermitGrantResult grant =
                grant_next_startup_permit(sem.queue, true);
            if (grant.ok && grant.granted.has_value()) {
                sem.queue = grant.queue;
                ++sem.in_flight;
                sem.granted_tickets.insert(grant.granted->ticket);
            }
        }
        // The grant may have gone to a waiter (an older request from
        // another App that this enqueue leapfrogged); wake it to re-check
        // its ticket.
        sem.cv.notify_all();
        // Wait for the permit. The predicate also abandons when the pool
        // drains mid-wait (generation no longer active).
        sem.cv.wait(lock, [&] {
            if (!active_flag_.load(std::memory_order_relaxed)) {
                return true;  // wake to abandon
            }
            return sem.granted_tickets.count(ticket) != 0;
        });
        if (!active_flag_.load(std::memory_order_relaxed)) {
            // The grant may have landed just before the drain flipped the
            // active flag: the permit was counted in sem.in_flight and must
            // be released back, or the slot leaks forever (each
            // drain-vs-replacement race would permanently eat one permit).
            if (sem.granted_tickets.erase(ticket) != 0 && sem.in_flight > 0) {
                --sem.in_flight;
            }
            lock.unlock();
            abandon();
            return;
        }
        // I own the permit; consume the grant record.
        sem.granted_tickets.erase(ticket);
    }

    // Spawn and READY through the same factory (same artifact, same
    // effective config — §8.3) into a fresh executor.
    std::shared_ptr<WorkerExecutor> replacement(new WorkerExecutor());
    replacement->set_event_notifier([weak = weak_from_this()] {
        if (std::shared_ptr<GenerationPool> p = weak.lock()) {
            {
                std::lock_guard<std::mutex> lock(p->mutex_);
                p->events_pending_ = true;
            }
            p->cv_.notify_all();
        }
    });
    std::string spawn_error;
    const bool ready = replacement->start(options_.factory, &spawn_error);
    {
        // Release the permit whether the spawn succeeded or not.
        std::unique_lock<std::mutex> lock(sem.mutex);
        if (sem.in_flight > 0) {
            --sem.in_flight;
        }
        // Grant the next waiting request (if any) now that a slot freed.
        if (sem.in_flight < sem.limit && !sem.queue.queued.empty()) {
            const StartupPermitGrantResult grant =
                grant_next_startup_permit(sem.queue, true);
            if (grant.ok && grant.granted.has_value()) {
                sem.queue = grant.queue;
                ++sem.in_flight;
                sem.granted_tickets.insert(grant.granted->ticket);
            }
        }
    }
    sem.cv.notify_all();

    if (!ready) {
        // A failed replacement is an instability: record it (the crash
        // budget applies) and re-schedule with the next backoff step.
        std::fprintf(stderr,
                     "capsid-host: generation pool %s: replacement spawn "
                     "failed: %s\n",
                     options_.application_id.c_str(), spawn_error.c_str());
        std::unique_lock<std::mutex> lock(mutex_);
        Slot& s = slots_[slot];
        s.replacement_in_flight = false;
        if (replacements_scheduled_ > 0) {
            --replacements_scheduled_;
        }
        --replacements_in_flight_;
        WorkerInstabilityObservation observation;
        observation.kind = WorkerInstabilityKind::kReplacementStartupFailure;
        observation.worker_generation = options_.generation_digest;
        observation.now_ms = steady_ms();
        observation.ready_workers_after_removal = ready_workers_locked();
        observation.target_ready_workers = options_.workers;
        observation.replacements_in_flight_for_app =
            static_cast<std::uint32_t>(replacements_scheduled_);
        observation.chosen_jitter_basis_points = 0;
        const WorkerRecoveryDecision decision = record_worker_instability(
            recovery_state_, policy_, lifecycle_, observation);
        recovery_state_ = decision.state;
        if (decision.ok && decision.schedule_replacement) {
            s.replacement_in_flight = true;
            ++replacements_scheduled_;
            replacement_due_ms_[slot] =
                steady_ms() + decision.replacement_delay_ms;
        }
        cv_.notify_all();
        return;
    }

    install_replacement(slot, std::move(replacement));
}

void GenerationPool::install_replacement(
    std::size_t slot, std::shared_ptr<WorkerExecutor> replacement) {
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
        if (state_ == State::kDraining) {
            s.executor->request_shutdown();
        }
        s.replacement_in_flight = false;
        if (replacements_scheduled_ > 0) {
            --replacements_scheduled_;
        }
        --replacements_in_flight_;
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
    startup_semaphore().cv.notify_all();
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
    // Wake permit waiters: they see the pool inactive and abandon.
    startup_semaphore().cv.notify_all();
    // Executor destructors run here, outside the mutex: each joins its
    // worker thread, which may be blocked on the pool mutex in the event
    // notifier.
}

}  // namespace capsid::host

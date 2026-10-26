#pragma once

// Deterministic single-threaded fiber scheduler for wrapped pthreads
// (docs/design.md §10). No OS threads are created: pthread_create enqueues a
// fiber, mutex/cond/join/sleep suspend the current fiber, and every choice of
// which ready fiber runs next draws from the Schedule stream. Virtual time
// advances only when no fiber is runnable, to the earliest timer.

#include "cosmos/event_sink.hpp"
#include "cosmos/faults.hpp"
#include "cosmos/memory.hpp"
#include "cosmos/random.hpp"
#include "cosmos/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <ucontext.h>
#include <vector>

namespace cosmos {

// 256 KiB per fiber: enough for test workloads doing printf/malloc, small
// enough for dozens of tasks. Allocated via __real_malloc so engine stacks
// never enter the tracked heap or the fault injector (Rule 7).
inline constexpr size_t kFiberStackSize = 256 * 1024;

struct Task {
    enum class State : uint8_t {
        Ready,
        Running,
        BlockedMutex,
        BlockedCond,
        BlockedJoin,
        Sleeping,
        Done,
    };

    ucontext_t ctx{};
    void* stack = nullptr;
    size_t stack_size = 0;
    void* (*func)(void*) = nullptr;
    void* arg = nullptr;
    void* retval = nullptr;
    pthread_t id = 0;
    // The simulated node this fiber belongs to; every node-scoped fault reads it (docs §9).
    NodeId node = 0;
    State state = State::Ready;
    bool detached = false;
    // Error to return when resumed via deadlock fallback. 0 means normal wake.
    int wake_error = 0;
    class Scheduler* owner = nullptr;
    // Tasks waiting to join this one. Moved to ready on exit.
    std::vector<Task*, RawRealAllocator<Task*>> joiners;
};

struct SimMutex {
    pthread_mutex_t* key = nullptr;
    uint64_t id = 0;
    bool locked = false;
    Task* owner = nullptr;
    std::vector<Task*, RawRealAllocator<Task*>> waiters;
};

struct SimCond {
    struct Waiter {
        Task* task = nullptr;
        pthread_mutex_t* mu = nullptr;
    };
    pthread_cond_t* key = nullptr;
    uint64_t id = 0;
    std::vector<Waiter, RawRealAllocator<Waiter>> waiters;
};

struct Timer {
    Time wakeup{};
    uint64_t seq = 0;
    Task* task = nullptr;
};

// Read-only state rows for the scheduler inspector (U3.3). They are produced on demand, outside
// the hot path, and never expose a Task*: a UI must key on the deterministic ids below.
struct TaskInfo {
    uint64_t id = 0;
    NodeId node = 0;
    Task::State state = Task::State::Ready;
    bool detached = false;
    uint64_t stack_size = 0;
    uint64_t joiners = 0;
};

struct MutexInfo {
    uint64_t id = 0;
    bool locked = false;
    uint64_t owner = 0;
    uint64_t waiters = 0;
};

struct CondInfo {
    uint64_t id = 0;
    uint64_t waiters = 0;
};

struct TimerInfo {
    uint64_t seq = 0;
    uint64_t task = 0;
    Time wakeup{};
};

struct DeadlineInfo {
    uint64_t id = 0;
    Time at{};
    bool armed = false;
};

// A virtual-time callback; `id` breaks ties so firing order follows arming order.
struct Deadline {
    Time at{};
    uint64_t id = 0;
    void (*fn)(void*) = nullptr;
    void* context = nullptr;
};

class Scheduler {
  public:
    explicit Scheduler(uint64_t schedule_seed, VirtualClock& clock);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // Spawning. Returns 0 or an errno-style error (EINVAL/EAGAIN). Never
    // switches: the child runs when the current fiber next blocks or drains.
    int spawn(pthread_t* out, void* (*start)(void*), void* arg);
    // Same, attributing every call the fiber makes to `node` (docs §9, §10.2).
    int spawn(NodeId node, pthread_t* out, void* (*start)(void*), void* arg);

    // Node of the running fiber; 0 for the main fiber and for engine callbacks.
    NodeId current_node() const { return current_ != nullptr ? current_->node : 0; }

    // Event identity for the current fiber: kNoTask when no fiber is running, kMainTask for the
    // main fiber, otherwise the pthread id. pthread_self() still reports 0 for the main fiber.
    uint64_t current_task_id() const;

    // Paused nodes stop at their next scheduler boundary; the resume re-readies their fibers.
    void pause_node(NodeId node);
    void resume_node(NodeId node);
    bool node_paused(NodeId node) const;

    // Fires at virtual `at`, before fibers woken at the same stamp; 0 means the table is full.
    using DeadlineFn = void (*)(void*);
    static constexpr size_t kMaxDeadlines = 64;
    uint64_t schedule_at(Time at, DeadlineFn fn, void* context);
    bool cancel_deadline(uint64_t id);
    size_t pending_deadlines() const { return deadline_count_; }

    // Joining. Blocks the caller until target completes unless target is
    // already done. Returns 0, ESRCH, EINVAL (detached), or EDEADLK (self).
    int join(pthread_t id, void** retval);

    int detach(pthread_t id);
    pthread_t self();
    // Terminates the calling fiber. Never returns for workers. Main calling
    // exit drains remaining fibers and terminates the process.
    [[noreturn]] void thread_exit(void* retval);

    int mutex_init(pthread_mutex_t* mu, const pthread_mutexattr_t* attr);
    int mutex_destroy(pthread_mutex_t* mu);
    int mutex_lock(pthread_mutex_t* mu);
    int mutex_unlock(pthread_mutex_t* mu);
    int mutex_trylock(pthread_mutex_t* mu);

    int cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr);
    int cond_destroy(pthread_cond_t* cond);
    int cond_wait(pthread_cond_t* cond, pthread_mutex_t* mu);
    int cond_signal(pthread_cond_t* cond);
    int cond_broadcast(pthread_cond_t* cond);

    void yield();

    // Blocking sleeps. Validation mirrors VirtualClock so wrappers stay thin.
    // nanosleep convention: 0 or -1 with errno. clock_nanosleep convention:
    // error number directly.
    int sleep_ns(const struct timespec* req, struct timespec* rem);
    int sleep_clock(clockid_t clk, int flags, const struct timespec* req, struct timespec* rem);

    // Drains all fibers. Called with the main fiber running. Returns with the
    // main fiber running; check deadlocked() for ABBA-style hangs.
    void run_until_quiescence();

    bool deadlocked() const { return deadlocked_; }
    Task* current_task() { return current_; }
    const Task* current_task() const { return current_; }
    Rng& schedule_rng() { return schedule_rng_; }
    const Rng& schedule_rng() const { return schedule_rng_; }
    size_t ready_count() const { return ready_.size(); }
    size_t task_count() const { return all_tasks_.size(); }
    size_t parked_count() const { return parked_.size(); }
    bool node_paused_at(size_t node) const { return node < paused_.size() && paused_[node]; }

    // Read-only enumerators. They allocate, so they are for the state inspector, never a wrapper
    // or a running-fiber hot path. Ordering is deterministic: task id, creation order, timer seq.
    std::vector<TaskInfo> task_infos() const;
    std::vector<MutexInfo> mutex_infos() const;
    std::vector<CondInfo> cond_infos() const;
    std::vector<TimerInfo> timer_infos() const;
    std::vector<DeadlineInfo> deadline_infos() const;

    // The inspection sink is owned by the universe. A null sink disables producer recording.
    void set_event_sink(EventSink* sink) { event_sink_ = sink; }
    EventSink* event_sink() const { return event_sink_; }
    // True after an installed breakpoint matched and the scheduler has reached a safe boundary.
    bool inspection_stop_requested() const;

    // Trampoline + exit path, public for makecontext.
    static void task_trampoline();
    void on_task_exit(Task* done);

  private:
    Task* find_task(pthread_t id);
    SimMutex* find_mutex(pthread_mutex_t* key);
    SimMutex& get_or_create_mutex(pthread_mutex_t* key);
    SimCond* find_cond(pthread_cond_t* key);
    SimCond& get_or_create_cond(pthread_cond_t* key);

    // Pops one ready fiber. Draws from the schedule stream only when the
    // choice matters (size > 1) so single-fiber runs never consume draws.
    Task* pick_next_ready();
    void switch_to(Task* next);
    void wake_joiners(Task* done);
    // Moves one cond waiter to runnable, reacquiring its mutex when free and
    // parking on that mutex queue otherwise. Callers pop FIFO for determinism.
    void deliver_cond_waiter(SimCond::Waiter waiter);
    bool has_blocked() const;
    void push_timer(Task* task, Time wakeup);
    // Single wake gate: a paused node's fiber parks instead of becoming runnable.
    void make_ready(Task* task);
    // Blocks the caller until its timer is due; deadlines may sit between timers.
    void sleep_on_timer(Task* self);
    // Fires every deadline due by `now`, earliest first; a callback may arm another.
    void fire_deadlines(Time now);
    void compact_deadlines();
    // Advances to the earliest timer and wakes everything due at that stamp.
    // Returns false when no timers or deadlines exist.
    bool advance_to_next_timer();
    // Advances until something is runnable; false when neither timers nor deadlines remain.
    bool advance_until_ready();
    [[noreturn]] void abort_deadlock(const char* what);

    Rng schedule_rng_;
    VirtualClock& clock_;
    Task main_task_;
    Task* current_ = nullptr;
    std::vector<Task*, RawRealAllocator<Task*>> ready_;
    std::vector<Task*, RawRealAllocator<Task*>> all_tasks_;
    std::vector<SimMutex, RawRealAllocator<SimMutex>> mutexes_;
    std::vector<SimCond, RawRealAllocator<SimCond>> conds_;
    std::vector<Timer, RawRealAllocator<Timer>> timers_;
    std::vector<bool> paused_;
    std::vector<Task*, RawRealAllocator<Task*>> parked_;
    std::array<Deadline, kMaxDeadlines> deadlines_{};
    size_t deadline_count_ = 0;
    uint64_t next_deadline_id_ = 1;
    uint64_t timer_seq_ = 0;
    uint64_t next_id_ = 2; // 1 is kMainTask; spawned ids never collide with it
    uint64_t next_mutex_id_ = 1;
    uint64_t next_cond_id_ = 1;
    bool deadlocked_ = false;
    Task* drain_requester_ = nullptr;
    EventSink* event_sink_ = nullptr;
};

} // namespace cosmos

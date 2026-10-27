#include "cosmos/task.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <time.h>
#include <unistd.h>

extern "C" {
void* __real_malloc(size_t size);
void __real_free(void* ptr);
}

namespace cosmos {
namespace {

bool timespec_valid(const struct timespec& ts) {
    return ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1'000'000'000L;
}

int64_t timespec_ns(const struct timespec& ts) {
    return add_sat(mul_sat(static_cast<int64_t>(ts.tv_sec), 1'000'000'000LL),
                   static_cast<int64_t>(ts.tv_nsec));
}

void write_zero(struct timespec* rem) {
    if (rem != nullptr) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
}

// Single OS thread, so a handoff set before each swap is always consumed by
// the fiber we switch to before any other switch can overwrite it.
thread_local Scheduler* t_active_scheduler = nullptr;

bool timer_holds(const std::vector<Timer, RawRealAllocator<Timer>>& timers, Task* self) {
    for (const Timer& timer : timers) {
        if (timer.task == self) return true;
    }
    return false;
}

// Event task identity. The main fiber's pthread id is 0, which the envelope reserves for absence.
uint64_t event_task_id(const Task* task) {
    if (task == nullptr) return kNoTask;
    return task->id == 0 ? kMainTask : static_cast<uint64_t>(task->id);
}

// One scheduler producer. Recording is a stamped assignment into a preallocated ring; it never
// allocates and never changes scheduler state. A disabled or unseeded sink is a no-op.
void record_scheduler(EventSink* sink, EventKind kind, Time at, uint64_t task, NodeId node,
                      EventPayload payload = {}, uint64_t correlation = kNoCorrelation) {
    if (sink == nullptr || !sink->enabled() || !sink->seeded()) return;
    Event event;
    event.at = at;
    event.kind = kind;
    event.node = node;
    event.task = task;
    event.correlation_id = correlation;
    event.payload = payload;
    sink->record(event);
}

void record_task(EventSink* sink, EventKind kind, Time at, const Task* task,
                 EventPayload payload = {}, uint64_t correlation = kNoCorrelation) {
    record_scheduler(sink, kind, at, event_task_id(task), task != nullptr ? task->node : kNoNode,
                     payload, correlation);
}

void sleep_block(Scheduler* sched);

} // namespace

Scheduler::Scheduler(uint64_t schedule_seed, VirtualClock& clock)
    : schedule_rng_(schedule_seed), clock_(clock) {
    main_task_.id = 0;
    main_task_.state = Task::State::Running;
    main_task_.owner = this;
    current_ = &main_task_;
}

Scheduler::~Scheduler() {
    for (Task* task : all_tasks_) {
        if (task == nullptr) continue;
        task->~Task();
        if (task->stack != nullptr) __real_free(task->stack);
        __real_free(task);
    }
}

Task* Scheduler::find_task(pthread_t id) {
    for (Task* task : all_tasks_) {
        if (task != nullptr && task->id == id) return task;
    }
    return nullptr;
}

SimMutex* Scheduler::find_mutex(pthread_mutex_t* key) {
    for (auto& mu : mutexes_) {
        if (mu.key == key) return &mu;
    }
    return nullptr;
}

SimMutex& Scheduler::get_or_create_mutex(pthread_mutex_t* key) {
    if (SimMutex* found = find_mutex(key)) return *found;
    SimMutex mu;
    mu.key = key;
    mu.id = next_mutex_id_++;
    mutexes_.push_back(mu);
    return mutexes_.back();
}

SimCond* Scheduler::find_cond(pthread_cond_t* key) {
    for (auto& cond : conds_) {
        if (cond.key == key) return &cond;
    }
    return nullptr;
}

SimCond& Scheduler::get_or_create_cond(pthread_cond_t* key) {
    if (SimCond* found = find_cond(key)) return *found;
    SimCond cond;
    cond.key = key;
    cond.id = next_cond_id_++;
    conds_.push_back(cond);
    return conds_.back();
}

Task* Scheduler::pick_next_ready() {
    // A matched breakpoint yields to the drain owner at the next scheduler choice. No ready task
    // is popped, so resuming the drain observes exactly the same ready order. This is the only
    // point where a paused universe hands control back to its inspector.
    if (inspection_stop_requested() && drain_requester_ != nullptr) {
        return drain_requester_;
    }
    if (ready_.size() == 1) {
        forced_choice_.reset();
        Task* task = ready_.back();
        ready_.pop_back();
        return task;
    }
    if (forced_choice_.has_value() && *forced_choice_ < ready_.size()) {
        const size_t index = static_cast<size_t>(*forced_choice_);
        forced_choice_.reset();
        forced_choice_used_ = true;
        Task* task = ready_[index];
        ready_.erase(ready_.begin() + static_cast<ptrdiff_t>(index));
        record_task(event_sink_, EventKind::scheduler_choice, clock_.now(), task,
                    EventPayload{.subject = static_cast<uint64_t>(index),
                                 .related = 0,
                                 .value = static_cast<uint64_t>(ready_.size()),
                                 .extra = 1,
                                 .amount_ns = 0});
        return task;
    }
    forced_choice_.reset();
    const uint64_t idx = schedule_rng_.range(0, ready_.size() - 1);
    Task* task = ready_[static_cast<size_t>(idx)];
    ready_.erase(ready_.begin() + static_cast<ptrdiff_t>(idx));
    record_task(event_sink_, EventKind::scheduler_choice, clock_.now(), task,
                EventPayload{.subject = static_cast<uint64_t>(idx),
                             .related = 0,
                             .value = static_cast<uint64_t>(ready_.size()),
                             .extra = 0,
                             .amount_ns = 0});
    return task;
}

void Scheduler::switch_to(Task* next) {
    Task* prev = current_;
    if (prev == next) {
        next->state = Task::State::Running;
        return;
    }
    record_task(event_sink_, EventKind::scheduler_run, clock_.now(), next,
                EventPayload{.value = static_cast<uint64_t>(ready_.size())});
    current_ = next;
    next->state = Task::State::Running;
    t_active_scheduler = this;
    swapcontext(&prev->ctx, &next->ctx);
}

bool Scheduler::inspection_stop_requested() const {
    return event_sink_ != nullptr && event_sink_->enabled() && event_sink_->observer_hit();
}

bool Scheduler::has_blocked() const {
    for (const Task* task : all_tasks_) {
        if (task == nullptr) continue;
        switch (task->state) {
        case Task::State::BlockedMutex:
        case Task::State::BlockedCond:
        case Task::State::BlockedJoin:
        case Task::State::Sleeping:
            return true;
        default:
            break;
        }
    }
    return false;
}

void Scheduler::push_timer(Task* task, Time wakeup) {
    timers_.push_back(Timer{wakeup, timer_seq_++, task});
    record_task(event_sink_, EventKind::scheduler_timer_arm, clock_.now(), task,
                EventPayload{.value = timer_seq_ - 1, .amount_ns = wakeup.ns}, 0);
}

void Scheduler::compact_deadlines() {
    size_t keep = 0;
    for (size_t i = 0; i < deadline_count_; ++i) {
        if (deadlines_[i].fn != nullptr) deadlines_[keep++] = deadlines_[i];
    }
    deadline_count_ = keep;
}

bool Scheduler::cancel_deadline(uint64_t id) {
    if (id == 0) return false;
    for (size_t i = 0; i < deadline_count_; ++i) {
        if (deadlines_[i].id == id && deadlines_[i].fn != nullptr) {
            deadlines_[i].fn = nullptr;
            compact_deadlines();
            record_scheduler(event_sink_, EventKind::scheduler_timer_cancel, clock_.now(), kNoTask,
                             kNoNode, EventPayload{.subject = id});
            return true;
        }
    }
    return false;
}

uint64_t Scheduler::schedule_at(Time at, DeadlineFn fn, void* context) {
    if (fn == nullptr) return 0;
    if (deadline_count_ == kMaxDeadlines) {
        compact_deadlines();
        if (deadline_count_ == kMaxDeadlines) return 0;
    }
    const uint64_t id = next_deadline_id_++;
    deadlines_[deadline_count_++] = Deadline{at, id, fn, context};
    return id;
}

void Scheduler::fire_deadlines(Time now) {
    // Pick the earliest due entry each pass; the bound stops a self-rearming callback.
    for (size_t fired = 0; fired <= kMaxDeadlines; ++fired) {
        size_t best = kMaxDeadlines;
        for (size_t i = 0; i < deadline_count_; ++i) {
            const Deadline& due = deadlines_[i];
            if (due.fn == nullptr || due.at > now) continue;
            if (best == kMaxDeadlines || due.at < deadlines_[best].at ||
                (due.at == deadlines_[best].at && due.id < deadlines_[best].id)) {
                best = i;
            }
        }
        if (best == kMaxDeadlines) break;
        const Deadline entry = deadlines_[best];
        deadlines_[best].fn = nullptr;
        record_scheduler(event_sink_, EventKind::scheduler_timer_fire, clock_.now(), kNoTask,
                         kNoNode, EventPayload{.subject = entry.id, .amount_ns = entry.at.ns});
        entry.fn(entry.context);
    }
    compact_deadlines();
}

bool Scheduler::node_paused(NodeId node) const { return node < paused_.size() && paused_[node]; }

void Scheduler::make_ready(Task* task) {
    task->state = Task::State::Ready;
    task->wake_error = 0;
    record_task(event_sink_, EventKind::scheduler_wake, clock_.now(), task,
                EventPayload{.value = paused_.empty() ? 0 : (node_paused(task->node) ? 1U : 0U)});
    if (node_paused(task->node)) {
        parked_.push_back(task);
    } else {
        ready_.push_back(task);
    }
}

void Scheduler::pause_node(NodeId node) {
    if (node >= paused_.size()) paused_.resize(static_cast<size_t>(node) + 1, false);
    paused_[node] = true;
    record_scheduler(event_sink_, EventKind::node_pause, clock_.now(), kNoTask, node);
    for (auto it = ready_.begin(); it != ready_.end();) {
        if ((*it)->node == node) {
            parked_.push_back(*it);
            it = ready_.erase(it);
        } else {
            ++it;
        }
    }
}

void Scheduler::resume_node(NodeId node) {
    if (node < paused_.size()) paused_[node] = false;
    record_scheduler(event_sink_, EventKind::node_resume, clock_.now(), kNoTask, node);
    for (auto it = parked_.begin(); it != parked_.end();) {
        if ((*it)->node == node) {
            ready_.push_back(*it);
            it = parked_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Scheduler::advance_until_ready() {
    while (ready_.empty() && (!timers_.empty() || deadline_count_ != 0)) {
        if (!advance_to_next_timer()) break;
    }
    return !ready_.empty();
}

void Scheduler::sleep_on_timer(Task* self) {
    self->state = Task::State::Sleeping;
    self->wake_error = 0;
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
        return;
    }
    // Advance past deadlines that sit before this timer.
    while (timer_holds(timers_, self) && ready_.empty()) {
        if (!advance_to_next_timer()) break;
    }
    if (!timer_holds(timers_, self)) {
        for (auto it = ready_.begin(); it != ready_.end(); ++it) {
            if (*it == self) {
                ready_.erase(it);
                break;
            }
        }
        // The wake landed on a pause: stay parked and let the drain reach the heal.
        if (node_paused(self->node)) {
            self->state = Task::State::Ready;
            if (drain_requester_ != nullptr) {
                switch_to(drain_requester_);
            } else if (!ready_.empty()) {
                switch_to(pick_next_ready());
            }
        }
        self->state = Task::State::Running;
        return;
    }
    if (!ready_.empty()) switch_to(pick_next_ready());
}

bool Scheduler::advance_to_next_timer() {
    bool found = false;
    Time earliest{};
    for (const Timer& timer : timers_) {
        if (!found || timer.wakeup < earliest) {
            earliest = timer.wakeup;
            found = true;
        }
    }
    for (size_t i = 0; i < deadline_count_; ++i) {
        if (deadlines_[i].fn == nullptr) continue;
        if (!found || deadlines_[i].at < earliest) {
            earliest = deadlines_[i].at;
            found = true;
        }
    }
    if (!found) return false;
    clock_.advance_to(earliest);
    // Events at this stamp land before the fibers woken at the same instant.
    fire_deadlines(earliest);
    std::vector<Timer, RawRealAllocator<Timer>> remaining;
    remaining.reserve(timers_.size());
    for (const Timer& timer : timers_) {
        if (timer.wakeup <= earliest) {
            record_task(event_sink_, EventKind::scheduler_timer_fire, clock_.now(), timer.task,
                        EventPayload{.subject = timer.seq, .amount_ns = timer.wakeup.ns});
            make_ready(timer.task);
        } else {
            remaining.push_back(timer);
        }
    }
    timers_.swap(remaining);
    return true;
}

[[noreturn]] void Scheduler::abort_deadlock(const char* what) {
    const char prefix[] = "cosmos: deadlock without drain in ";
    (void)::write(2, prefix, sizeof(prefix) - 1);
    size_t len = 0;
    while (what[len] != '\0')
        ++len;
    (void)::write(2, what, len);
    (void)::write(2, "\n", 1);
    ::abort();
}

int Scheduler::spawn(pthread_t* out, void* (*start)(void*), void* arg) {
    return spawn(0, out, start, arg);
}

int Scheduler::spawn(NodeId node, pthread_t* out, void* (*start)(void*), void* arg) {
    if (out == nullptr || start == nullptr) return EINVAL;
    void* task_mem = __real_malloc(sizeof(Task));
    if (task_mem == nullptr) return EAGAIN;
    void* stack = __real_malloc(kFiberStackSize);
    if (stack == nullptr) {
        __real_free(task_mem);
        return EAGAIN;
    }
    auto* task = new (task_mem) Task();
    task->stack = stack;
    task->stack_size = kFiberStackSize;
    task->node = node;
    task->func = start;
    task->arg = arg;
    do {
        task->id = static_cast<pthread_t>(next_id_++);
    } while (task->id == 0);
    task->state = Task::State::Ready;
    task->owner = this;
    record_task(event_sink_, EventKind::scheduler_spawn, clock_.now(), task,
                EventPayload{.value = static_cast<uint64_t>(all_tasks_.size()),
                             .extra = static_cast<uint64_t>(kFiberStackSize)});
    if (::getcontext(&task->ctx) != 0) {
        task->~Task();
        __real_free(stack);
        __real_free(task_mem);
        return EAGAIN;
    }
    task->ctx.uc_stack.ss_sp = stack;
    task->ctx.uc_stack.ss_size = kFiberStackSize;
    task->ctx.uc_stack.ss_flags = 0;
    task->ctx.uc_link = nullptr;
    ::makecontext(&task->ctx, reinterpret_cast<void (*)()>(&Scheduler::task_trampoline), 0);
    make_ready(task);
    all_tasks_.push_back(task);
    *out = task->id;
    return 0;
}

int Scheduler::join(pthread_t id, void** retval) {
    Task* target = find_task(id);
    if (target == nullptr) return ESRCH;
    if (target->id == current_->id) return EDEADLK;
    if (target->detached) return EINVAL;
    record_task(event_sink_, EventKind::scheduler_join, clock_.now(), current_,
                EventPayload{.related = static_cast<uint64_t>(target->id)});
    if (target->state == Task::State::Done) {
        if (retval != nullptr) *retval = target->retval;
        return 0;
    }
    Task* self = current_;
    target->joiners.push_back(self);
    self->state = Task::State::BlockedJoin;
    self->wake_error = 0;
    record_task(event_sink_, EventKind::scheduler_block, clock_.now(), self,
                EventPayload{.related = static_cast<uint64_t>(target->id),
                             .value = static_cast<uint64_t>(Task::State::BlockedJoin)});
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (advance_until_ready()) {
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            switch_to(drain_requester_);
        } else {
            auto& joiners = target->joiners;
            for (auto it = joiners.begin(); it != joiners.end(); ++it) {
                if (*it == self) {
                    joiners.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
            abort_deadlock("pthread_join");
        }
    }
    if (self->wake_error != 0) return self->wake_error;
    if (retval != nullptr) *retval = target->retval;
    return 0;
}

int Scheduler::detach(pthread_t id) {
    Task* target = find_task(id);
    if (target == nullptr) return ESRCH;
    if (target->detached) return EINVAL;
    target->detached = true;
    record_task(event_sink_, EventKind::scheduler_detach, clock_.now(), current_,
                EventPayload{.related = static_cast<uint64_t>(target->id)});
    return 0;
}

pthread_t Scheduler::self() { return current_->id; }

[[noreturn]] void Scheduler::thread_exit(void* retval) {
    Task* self = current_;
    if (self == &main_task_) {
        main_task_.retval = retval;
        main_task_.state = Task::State::Done;
        while (!ready_.empty() || !timers_.empty() || deadline_count_ != 0) {
            if (ready_.empty()) {
                advance_to_next_timer();
                continue;
            }
            switch_to(pick_next_ready());
        }
        ::exit(has_blocked() ? 1 : 0);
    }
    self->retval = retval;
    on_task_exit(self);
    ::abort();
}

int Scheduler::mutex_init(pthread_mutex_t* mu, const pthread_mutexattr_t* attr) {
    (void)attr;
    if (mu == nullptr) return EINVAL;
    SimMutex* existing = find_mutex(mu);
    if (existing != nullptr) {
        if (existing->locked || !existing->waiters.empty()) return EBUSY;
        existing->locked = false;
        existing->owner = nullptr;
        return 0;
    }
    SimMutex entry;
    entry.key = mu;
    entry.id = next_mutex_id_++;
    mutexes_.push_back(entry);
    return 0;
}

int Scheduler::mutex_destroy(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    for (auto it = mutexes_.begin(); it != mutexes_.end(); ++it) {
        if (it->key != mu) continue;
        if (it->locked || !it->waiters.empty()) return EBUSY;
        mutexes_.erase(it);
        return 0;
    }
    return 0;
}

int Scheduler::mutex_lock(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    SimMutex& entry = get_or_create_mutex(mu);
    Task* self = current_;
    if (!entry.locked) {
        entry.locked = true;
        entry.owner = self;
        record_scheduler(event_sink_, EventKind::scheduler_mutex_lock, clock_.now(), self->id,
                         self->node, EventPayload{.subject = entry.id, .related = self->id});
        return 0;
    }
    if (entry.owner == self) return EDEADLK;
    entry.waiters.push_back(self);
    self->state = Task::State::BlockedMutex;
    self->wake_error = 0;
    record_scheduler(
        event_sink_, EventKind::scheduler_mutex_contend, clock_.now(), self->id, self->node,
        EventPayload{.subject = entry.id,
                     .related =
                         entry.owner != nullptr ? static_cast<uint64_t>(entry.owner->id) : kNoTask,
                     .value = static_cast<uint64_t>(entry.waiters.size())});
    record_task(event_sink_, EventKind::scheduler_block, clock_.now(), self,
                EventPayload{.subject = entry.id,
                             .value = static_cast<uint64_t>(Task::State::BlockedMutex)});
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (advance_until_ready()) {
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            switch_to(drain_requester_);
        } else {
            auto& waiters = entry.waiters;
            for (auto it = waiters.begin(); it != waiters.end(); ++it) {
                if (*it == self) {
                    waiters.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
            abort_deadlock("pthread_mutex_lock");
        }
    }
    if (self->wake_error != 0) return self->wake_error;
    return 0;
}

int Scheduler::mutex_unlock(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    SimMutex* entry = find_mutex(mu);
    if (entry == nullptr || !entry->locked || entry->owner != current_) return EPERM;
    uint64_t next_owner = kNoTask;
    if (!entry->waiters.empty()) {
        Task* waiter = entry->waiters.front();
        entry->waiters.erase(entry->waiters.begin());
        entry->owner = waiter;
        next_owner = event_task_id(waiter);
        make_ready(waiter);
    } else {
        entry->locked = false;
        entry->owner = nullptr;
    }
    record_scheduler(event_sink_, EventKind::scheduler_mutex_unlock, clock_.now(),
                     event_task_id(current_), current_->node,
                     EventPayload{.subject = entry->id, .related = next_owner});
    Task* self = current_;
    make_ready(self);
    switch_to(pick_next_ready());
    return 0;
}

int Scheduler::mutex_trylock(pthread_mutex_t* mu) {
    if (mu == nullptr) return EINVAL;
    SimMutex& entry = get_or_create_mutex(mu);
    if (!entry.locked) {
        entry.locked = true;
        entry.owner = current_;
        return 0;
    }
    return EBUSY;
}

int Scheduler::cond_init(pthread_cond_t* cond, const pthread_condattr_t* attr) {
    (void)attr;
    if (cond == nullptr) return EINVAL;
    if (find_cond(cond) != nullptr) return EBUSY;
    SimCond entry;
    entry.key = cond;
    entry.id = next_cond_id_++;
    conds_.push_back(entry);
    return 0;
}

int Scheduler::cond_destroy(pthread_cond_t* cond) {
    if (cond == nullptr) return EINVAL;
    for (auto it = conds_.begin(); it != conds_.end(); ++it) {
        if (it->key != cond) continue;
        if (!it->waiters.empty()) return EBUSY;
        conds_.erase(it);
        return 0;
    }
    return 0;
}

int Scheduler::cond_wait(pthread_cond_t* cond, pthread_mutex_t* mu) {
    if (cond == nullptr || mu == nullptr) return EINVAL;
    SimCond& cond_entry = get_or_create_cond(cond);
    SimMutex& mu_entry = get_or_create_mutex(mu);
    Task* self = current_;
    if (!mu_entry.locked || mu_entry.owner != self) return EPERM;
    record_scheduler(event_sink_, EventKind::scheduler_cond_wait, clock_.now(), event_task_id(self),
                     self->node, EventPayload{.subject = cond_entry.id, .related = mu_entry.id});
    if (!mu_entry.waiters.empty()) {
        Task* waiter = mu_entry.waiters.front();
        mu_entry.waiters.erase(mu_entry.waiters.begin());
        mu_entry.owner = waiter;
        make_ready(waiter);
    } else {
        mu_entry.locked = false;
        mu_entry.owner = nullptr;
    }
    cond_entry.waiters.push_back(SimCond::Waiter{self, mu});
    self->state = Task::State::BlockedCond;
    self->wake_error = 0;
    record_task(event_sink_, EventKind::scheduler_block, clock_.now(), self,
                EventPayload{.subject = cond_entry.id,
                             .value = static_cast<uint64_t>(Task::State::BlockedCond)});
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (advance_until_ready()) {
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            switch_to(drain_requester_);
        } else {
            auto& waiters = cond_entry.waiters;
            for (auto it = waiters.begin(); it != waiters.end(); ++it) {
                if (it->task == self) {
                    waiters.erase(it);
                    break;
                }
            }
            self->state = Task::State::Running;
            abort_deadlock("pthread_cond_wait");
        }
    }
    if (self->wake_error != 0) return self->wake_error;
    return 0;
}

void Scheduler::deliver_cond_waiter(SimCond::Waiter waiter) {
    SimMutex& mu_entry = get_or_create_mutex(waiter.mu);
    Task* task = waiter.task;
    if (!mu_entry.locked) {
        mu_entry.locked = true;
        mu_entry.owner = task;
        make_ready(task);
    } else {
        mu_entry.waiters.push_back(task);
        task->state = Task::State::BlockedMutex;
        task->wake_error = 0;
    }
}

int Scheduler::cond_signal(pthread_cond_t* cond) {
    if (cond == nullptr) return EINVAL;
    uint64_t woken = 0;
    SimCond* entry = find_cond(cond);
    if (entry != nullptr && !entry->waiters.empty()) {
        SimCond::Waiter waiter = entry->waiters.front();
        entry->waiters.erase(entry->waiters.begin());
        woken = event_task_id(waiter.task);
        record_scheduler(event_sink_, EventKind::scheduler_cond_signal, clock_.now(),
                         event_task_id(current_), current_->node,
                         EventPayload{.subject = entry->id, .related = woken, .value = 1});
        deliver_cond_waiter(waiter);
    } else {
        record_scheduler(event_sink_, EventKind::scheduler_cond_signal, clock_.now(),
                         event_task_id(current_), current_->node,
                         EventPayload{.subject = entry != nullptr ? entry->id : 0, .value = 0});
    }
    Task* self = current_;
    make_ready(self);
    switch_to(pick_next_ready());
    return 0;
}

int Scheduler::cond_broadcast(pthread_cond_t* cond) {
    if (cond == nullptr) return EINVAL;
    uint64_t woken = 0;
    SimCond* entry = find_cond(cond);
    if (entry != nullptr && !entry->waiters.empty()) {
        std::vector<SimCond::Waiter, RawRealAllocator<SimCond::Waiter>> waking;
        waking.swap(entry->waiters);
        woken = static_cast<uint64_t>(waking.size());
        record_scheduler(event_sink_, EventKind::scheduler_cond_broadcast, clock_.now(),
                         event_task_id(current_), current_->node,
                         EventPayload{.subject = entry->id, .value = woken});
        for (SimCond::Waiter waiter : waking)
            deliver_cond_waiter(waiter);
    } else {
        record_scheduler(event_sink_, EventKind::scheduler_cond_broadcast, clock_.now(),
                         event_task_id(current_), current_->node,
                         EventPayload{.subject = entry != nullptr ? entry->id : 0, .value = 0});
    }
    Task* self = current_;
    make_ready(self);
    switch_to(pick_next_ready());
    return 0;
}

void Scheduler::yield() {
    Task* self = current_;
    record_task(event_sink_, EventKind::scheduler_yield, clock_.now(), self);
    make_ready(self);
    switch_to(pick_next_ready());
}

int Scheduler::sleep_ns(const struct timespec* req, struct timespec* rem) {
    if (req == nullptr) {
        errno = EFAULT;
        return -1;
    }
    if (!timespec_valid(*req)) {
        errno = EINVAL;
        return -1;
    }
    const int64_t ns = timespec_ns(*req);
    if (ns <= 0) {
        write_zero(rem);
        return 0;
    }
    Task* self = current_;
    push_timer(self, clock_.now() + Duration{ns});
    sleep_on_timer(self);
    write_zero(rem);
    return 0;
}

int Scheduler::sleep_clock(clockid_t clk, int flags, const struct timespec* req,
                           struct timespec* rem) {
    if (req == nullptr) return EFAULT;
    if (!timespec_valid(*req)) return EINVAL;
    if (flags != 0 && flags != TIMER_ABSTIME) return EINVAL;
    bool realtime = false;
    switch (clk) {
    case CLOCK_MONOTONIC:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
#ifdef CLOCK_MONOTONIC_COARSE
    case CLOCK_MONOTONIC_COARSE:
#endif
        realtime = false;
        break;
    case CLOCK_REALTIME:
#ifdef CLOCK_REALTIME_COARSE
    case CLOCK_REALTIME_COARSE:
#endif
        realtime = true;
        break;
    default:
        return EINVAL;
    }
    if (flags == TIMER_ABSTIME) {
        // clock_nanosleep(2): the remainder is unused for an absolute sleep, so leave it untouched.
        const int64_t deadline_ns = timespec_ns(*req);
        const Time target =
            realtime ? Time{sub_sat(deadline_ns, clock_.realtime_epoch_ns())} : Time{deadline_ns};
        if (target <= clock_.now()) {
            return 0;
        }
        Task* self = current_;
        push_timer(self, target);
        sleep_on_timer(self);
        return 0;
    }
    const int64_t ns = timespec_ns(*req);
    if (ns <= 0) {
        write_zero(rem);
        return 0;
    }
    Task* self = current_;
    push_timer(self, clock_.now() + Duration{ns});
    sleep_on_timer(self);
    write_zero(rem);
    return 0;
}

void Scheduler::run_until_quiescence() {
    if (drain_requester_ != nullptr) return;
    drain_requester_ = current_;
    deadlocked_ = false;
    while (true) {
        // A matched inspection breakpoint pauses the drain at a scheduler boundary. The paused
        // tasks keep their state; a later call resumes exactly where this one stopped.
        if (inspection_stop_requested()) {
            drain_requester_ = nullptr;
            return;
        }
        if (ready_.empty() && timers_.empty() && deadline_count_ == 0) break;
        if (ready_.empty()) {
            advance_to_next_timer();
            continue;
        }
        switch_to(pick_next_ready());
    }
    deadlocked_ = has_blocked();
    drain_requester_ = nullptr;
    if (deadlocked_) {
        record_scheduler(event_sink_, EventKind::scheduler_deadlock, clock_.now(), kNoTask, kNoNode,
                         EventPayload{.value = static_cast<uint64_t>(task_count())});
    }
}

void Scheduler::wake_joiners(Task* done) {
    for (Task* joiner : done->joiners) {
        make_ready(joiner);
    }
    done->joiners.clear();
}

void Scheduler::task_trampoline() {
    Scheduler* sched = t_active_scheduler;
    // The switcher set current_ before swapping in, so this is us.
    Task* self = sched->current_;
    void* retval = self->func(self->arg);
    self->retval = retval;
    sched->on_task_exit(self);
    ::abort();
}

void Scheduler::on_task_exit(Task* done) {
    done->state = Task::State::Done;
    record_task(event_sink_, EventKind::scheduler_exit, clock_.now(), done,
                EventPayload{.value = static_cast<uint64_t>(done->joiners.size())});
    wake_joiners(done);
    if (!ready_.empty()) {
        switch_to(pick_next_ready());
    } else if (advance_until_ready()) {
        switch_to(pick_next_ready());
    } else {
        if (drain_requester_ != nullptr) {
            deadlocked_ = has_blocked();
            switch_to(drain_requester_);
        } else if (main_task_.state == Task::State::Done) {
            ::exit(has_blocked() ? 1 : 0);
        } else if (has_blocked()) {
            abort_deadlock("fiber exit");
        } else {
            abort_deadlock("fiber exit (quiescent without drain)");
        }
    }
    ::abort();
}

uint64_t Scheduler::current_task_id() const { return event_task_id(current_); }

std::vector<TaskInfo> Scheduler::task_infos() const {
    std::vector<TaskInfo> infos;
    infos.reserve(all_tasks_.size() + 1);
    infos.push_back(TaskInfo{kMainTask, main_task_.node, main_task_.state, main_task_.detached,
                             static_cast<uint64_t>(main_task_.stack_size),
                             static_cast<uint64_t>(main_task_.joiners.size())});
    for (const Task* task : all_tasks_) {
        if (task == nullptr) continue;
        infos.push_back(TaskInfo{event_task_id(task), task->node, task->state, task->detached,
                                 static_cast<uint64_t>(task->stack_size),
                                 static_cast<uint64_t>(task->joiners.size())});
    }
    std::sort(infos.begin(), infos.end(),
              [](const TaskInfo& left, const TaskInfo& right) { return left.id < right.id; });
    return infos;
}

std::vector<MutexInfo> Scheduler::mutex_infos() const {
    std::vector<MutexInfo> infos;
    infos.reserve(mutexes_.size());
    for (const SimMutex& mu : mutexes_) {
        infos.push_back(MutexInfo{
            mu.id, mu.locked, mu.owner != nullptr ? static_cast<uint64_t>(mu.owner->id) : kNoTask,
            static_cast<uint64_t>(mu.waiters.size())});
    }
    std::sort(infos.begin(), infos.end(),
              [](const MutexInfo& left, const MutexInfo& right) { return left.id < right.id; });
    return infos;
}

std::vector<CondInfo> Scheduler::cond_infos() const {
    std::vector<CondInfo> infos;
    infos.reserve(conds_.size());
    for (const SimCond& cond : conds_) {
        infos.push_back(CondInfo{cond.id, static_cast<uint64_t>(cond.waiters.size())});
    }
    std::sort(infos.begin(), infos.end(),
              [](const CondInfo& left, const CondInfo& right) { return left.id < right.id; });
    return infos;
}

std::vector<TimerInfo> Scheduler::timer_infos() const {
    std::vector<TimerInfo> infos;
    infos.reserve(timers_.size());
    for (const Timer& timer : timers_) {
        infos.push_back(TimerInfo{timer.seq, event_task_id(timer.task), timer.wakeup});
    }
    std::sort(infos.begin(), infos.end(),
              [](const TimerInfo& left, const TimerInfo& right) { return left.seq < right.seq; });
    return infos;
}

std::vector<DeadlineInfo> Scheduler::deadline_infos() const {
    std::vector<DeadlineInfo> infos;
    infos.reserve(deadline_count_);
    for (size_t index = 0; index < deadline_count_; ++index) {
        infos.push_back(DeadlineInfo{deadlines_[index].id, deadlines_[index].at,
                                     deadlines_[index].fn != nullptr});
    }
    std::sort(infos.begin(), infos.end(), [](const DeadlineInfo& left, const DeadlineInfo& right) {
        return left.id < right.id;
    });
    return infos;
}

} // namespace cosmos

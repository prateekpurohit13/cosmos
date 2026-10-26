#pragma once

#include "cosmos/event_sink.hpp"
#include "cosmos/fault_injector.hpp"
#include "cosmos/faults.hpp"
#include "cosmos/memory.hpp"
#include "cosmos/net.hpp"
#include "cosmos/node_registry.hpp"
#include "cosmos/random.hpp"
#include "cosmos/storage.hpp"
#include "cosmos/task.hpp"
#include "cosmos/telemetry.hpp"
#include "cosmos/time.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cosmos {

// Default fault-stream seed when a Simulator is built without an explicit one, so runs are
// reproducible out of the box. ASCII "Cosmos1".
inline constexpr uint64_t kDefaultUniverseSeed = 0x436F736D6F7331ULL;

// An injector carrying its own validating factory governs its construction, and the raw slot
// setters below are withdrawn for it: emplacing one directly would reopen the wrong-seed and
// second-install paths install_faults exists to close.
template <typename I>
concept HasFactory = requires { I::create; };

// Every instantiation gets its own current_sim_, so only the Simulator alias below is reachable
// through a wrapped syscall.
template <typename Injector> class BasicSimulator {
  public:
    explicit BasicSimulator(uint64_t seed = kDefaultUniverseSeed)
        : seed_(seed), user_rng_(stream_seed(seed, StreamDomain::User)),
          swarm_rng_(stream_seed(seed, StreamDomain::Swarm)),
          workload_rng_(stream_seed(seed, StreamDomain::Workload)),
          scheduler_(stream_seed(seed, StreamDomain::Schedule), clock_), net_(scheduler_, clock_) {}

    ~BasicSimulator() {
        if (current_sim_ == this) {
            current_sim_ = nullptr;
        }
    }

    // Copying would leave two objects claiming the same thread_local slot, and destroying either
    // one would clear it while the other is still current.
    BasicSimulator(const BasicSimulator&) = delete;
    BasicSimulator& operator=(const BasicSimulator&) = delete;

    static BasicSimulator* current() { return current_sim_; }

    static bool has_current() { return current_sim_ != nullptr; }

    static void set_current(BasicSimulator* sim) { current_sim_ = sim; }

    TrackedHeap& heap() { return heap_; }
    const TrackedHeap& heap() const { return heap_; }

    Scheduler& scheduler() { return scheduler_; }
    const Scheduler& scheduler() const { return scheduler_; }

    Net& net() { return net_; }
    const Net& net() const { return net_; }

    VirtualClock& clock() { return clock_; }
    const VirtualClock& clock() const { return clock_; }

    NodeRegistry& nodes() { return nodes_; }
    const NodeRegistry& nodes() const { return nodes_; }

    StorageModel& storage() { return storage_; }
    const StorageModel& storage() const { return storage_; }

    // Crashes the node and closes its endpoints; storage is discarded at reboot.
    bool crash_node(NodeId node) {
        if (!nodes_.crash(node)) return false;
        net_.crash_node(node);
        record_node_event(EventKind::node_crash, node);
        return true;
    }

    // Discards un-synced storage, then asks the injector whether the last synced sector tears.
    bool reboot_node(NodeId node) {
        if (!nodes_.reboot(node)) return false;
        bool torn = false;
        if (injector_.has_value()) {
            if constexpr (requires {
                              injector_->decide_event(FaultClass::Storage, SiteId::torn_write);
                          }) {
                torn = injector_->decide_event(FaultClass::Storage, SiteId::torn_write) ==
                       FaultKind::TornWrite;
            }
        }
        storage_.reboot(node, torn);
        record_node_event(EventKind::node_reboot, node);
        return true;
    }

    Time now() const { return clock_.now(); }
    void advance_time(Duration d) { clock_.advance(d); }

    // Deterministic stdin for a workload that reads fd 0. The buffer is copied into storage that
    // bypasses the tracked heap, so configuring it while a universe is current cannot perturb the
    // allocation counters a fault test is trying to pin. Empty means EOF, never the host's stdin.
    void set_stdin(std::string_view bytes) {
        stdin_.assign(bytes.begin(), bytes.end());
        stdin_offset_ = 0;
        stdin_owned_ = true;
        struct stat st{};
        stdin_identity_ = {};
        if (::fstat(STDIN_FILENO, &st) == 0) {
            stdin_identity_ =
                StdinIdentity{true, st.st_dev, st.st_ino, static_cast<mode_t>(st.st_mode & S_IFMT)};
        }
    }

    void clear_stdin() {
        stdin_.clear();
        stdin_offset_ = 0;
        stdin_owned_ = false;
        stdin_identity_ = {};
    }

    std::size_t stdin_remaining() const {
        assert(stdin_offset_ <= stdin_.size());
        return stdin_.size() - stdin_offset_;
    }

    // True while fd 0 belongs to the universe. The default is the host descriptor, so an
    // application that never installs a workload stream keeps its normal stdin. set_stdin() arms
    // ownership; close(0) and a successful open() that returns 0 release it. clear_stdin()
    // uninstalls the buffer and returns fd 0 to the host. dup2() redirection is not modelled.
    bool stdin_owned() const { return stdin_owned_; }
    void release_stdin() {
        stdin_owned_ = false;
        stdin_identity_ = {};
    }

    // Guard against descriptor replacement we do not wrap (openat, creat, dup2, pipe, ...).
    // If fd 0 no longer names the descriptor that set_stdin() captured, reads must go to the host.
    // An absent descriptor never matches, including the case where set_stdin() could not capture an
    // identity because fd 0 was already closed: the read then reaches the host and reports EBADF,
    // which is what POSIX requires of a read on a closed descriptor.
    bool stdin_descriptor_matches() const {
        struct stat st{};
        if (::fstat(STDIN_FILENO, &st) != 0 || !stdin_identity_.valid) return false;
        return st.st_dev == stdin_identity_.dev && st.st_ino == stdin_identity_.ino &&
               (st.st_mode & S_IFMT) == stdin_identity_.mode;
    }

    // Engine entry point used by __wrap_read for fd 0: POSIX-style partial reads, then EOF.
    ssize_t read_stdin(void* buffer, std::size_t count) {
        if (count == 0) return 0;
        if (buffer == nullptr) {
            errno = EFAULT;
            return -1;
        }
        const std::size_t remaining = stdin_remaining();
        if (remaining == 0) return 0;
        constexpr std::size_t kMaxRead =
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
        const std::size_t chunk = std::min({count, remaining, kMaxRead});
        std::memcpy(buffer, stdin_.data() + stdin_offset_, chunk);
        stdin_offset_ += chunk;
        return static_cast<ssize_t>(chunk);
    }

    // Node the running fiber belongs to; 0 for the main fiber and engine callbacks.
    NodeId current_node() const { return scheduler_.current_node(); }

    // Spawns a fiber whose wrapped calls are attributed to `node`.
    int spawn_node(NodeId node, pthread_t* out, void* (*start)(void*), void* arg) {
        return scheduler_.spawn(node, out, start, arg);
    }

    // Arms a callback at virtual `when`; false when the fixed deadline table is full.
    bool at(Time when, void (*fn)(void*), void* context) {
        return scheduler_.schedule_at(when, fn, context) != 0;
    }
    bool cancel_at(uint64_t id) { return scheduler_.cancel_deadline(id); }

    uint64_t seed() const { return seed_; }

    // The campaign's verify mode applies this when a body installs a plan without trace options.
    void set_default_trace_options(TraceOptions options) { default_trace_options_ = options; }

    [[nodiscard]] std::expected<void, ConfigProblem>
    install_faults(FaultConfig cfg, uint32_t node_count = 1, TraceOptions trace_options = {}) {
        // A second install would fork stream positions, counters and the ledger against one run.
        if (injector_.has_value()) {
            return std::unexpected(
                ConfigProblem{ConfigError::InjectorAlreadyInstalled, std::nullopt});
        }

        // Campaign verify turns tracing on for bodies that do not ask for it themselves.
        if (!trace_options.enabled && default_trace_options_.enabled) {
            trace_options = default_trace_options_;
        }
        // Seeding from the universe seed instead of the Fault domain would collide five of six
        // class sub-streams with the domain streams.
        auto made = Injector::create(std::move(cfg), stream_seed(seed_, StreamDomain::Fault),
                                     node_count, clock_, trace_options, episode_deps());
        if (!made.has_value()) return std::unexpected(made.error());
        // Nothing is crashed before the first install, so the growth path is the only one taken.
        (void)nodes_.set_node_count(node_count);
        injector_.emplace(std::move(*made));
        // The trace names its own universe, so Scenario::replay needs no seed argument.
        injector_->set_universe_seed(seed_);
        // Scheduled episodes arm here: the injector is stable only after emplacement.
        if (!injector_->arm_scheduled()) {
            injector_.reset();
            return std::unexpected(
                ConfigProblem{ConfigError::TooManyScheduledEpisodes, std::nullopt});
        }
        return {};
    }

    // User-visible randomness (getrandom/random values). Never draws for fault decisions (Rule 1).
    Rng& user_rng() { return user_rng_; }
    const Rng& user_rng() const { return user_rng_; }

    // Universe config sampling (Rule 1: the Fault domain is untouched by it).
    Rng& swarm_rng() { return swarm_rng_; }
    const Rng& swarm_rng() const { return swarm_rng_; }

    // Input generation (docs/design.md §11). A workload draws every generated value from here, so
    // changing fault, schedule, or swarm decisions, or application getrandom calls, cannot shift
    // the data stream.
    Rng& workload_rng() { return workload_rng_; }
    const Rng& workload_rng() const { return workload_rng_; }

    bool has_injector() const { return injector_.has_value(); }

    // Inspection event capture. Call enable_event_capture() before a universe starts: it allocates
    // the fixed ring once, outside the wrapped allocation path. A disabled sink is a no-op for
    // every producer, so instrumentation costs one branch when capture is off.
    EventSink& event_sink() { return event_sink_; }
    const EventSink& event_sink() const { return event_sink_; }
    EventSink* event_sink_or_null() { return event_sink_.enabled() ? &event_sink_ : nullptr; }
    const EventSink* event_sink_or_null() const {
        return event_sink_.enabled() ? &event_sink_ : nullptr;
    }

    void enable_event_capture(size_t capacity) {
        if (capacity == 0) return;
        // A hook installed on the disabled sink is legal (see EventSink); enabling capture must
        // not silently discard it.
        const EventObserver observer = event_sink_.observer();
        void* observer_context = event_sink_.observer_context();
        event_sink_ = EventSink(capacity, seed_);
        if (observer != nullptr) event_sink_.set_observer(observer, observer_context);
        scheduler_.set_event_sink(&event_sink_);
        net_.set_event_sink(&event_sink_);
        storage_.set_event_sink(&event_sink_, &clock_);
        heap_.set_event_sink(&event_sink_, &clock_);
        telemetry_.attach(&event_sink_, &clock_);
        sink_attached_ = true;
    }

    // Records a final summary leak event when the universe leaves allocations behind. The stream
    // cannot carry a per-allocation table without a second hot-path map; the artifact state does.
    void record_final_events() {
        if (!event_sink_.enabled() || !event_sink_.seeded()) return;
        const HeapStats& stats = heap_.stats();
        if (stats.active_allocations == 0) return;
        Event event;
        event.at = clock_.now();
        event.kind = EventKind::memory_leak;
        event.node = kNoNode;
        event.payload.value = static_cast<uint64_t>(stats.active_allocations);
        event.payload.extra = static_cast<uint64_t>(stats.active_bytes);
        event_sink_.record(event);
    }

    bool event_capture_attached() const { return sink_attached_; }

    Telemetry& telemetry() { return telemetry_; }
    const Telemetry& telemetry() const { return telemetry_; }

    // Deterministic label registry for framework application events (always/sometimes/reachable).
    // Telemetry has its own typed series registry; these labels are the check and coverage ids
    // that already exist in ScenarioReport, so an event can be joined to the report without
    // putting a string on the record path.
    uint64_t application_label(std::string_view name) {
        if (name.empty()) return 0;
        for (size_t index = 0; index < application_labels_.size(); ++index) {
            const RawString& label = application_labels_[index];
            if (label.size() == name.size() &&
                std::string_view(label.data(), label.size()) == name) {
                return index + 1;
            }
        }
        application_labels_.emplace_back(name.begin(), name.end());
        return application_labels_.size();
    }
    const std::vector<RawString, RawRealAllocator<RawString>>& application_labels() const {
        return application_labels_;
    }

    void record_application_event(EventKind kind, std::string_view label, uint64_t value,
                                  int64_t amount_ns = 0) {
        EventSink* sink = event_sink_or_null();
        if (sink == nullptr || !sink->seeded()) return;
        Event event;
        event.at = now();
        event.kind = kind;
        event.node = current_node();
        event.task = scheduler_.current_task_id();
        event.payload.subject = application_label(label);
        event.payload.value = value;
        event.payload.amount_ns = amount_ns;
        sink->record(event);
    }

  private:
    void record_node_event(EventKind kind, NodeId node) {
        if (!event_sink_.enabled() || !event_sink_.seeded()) return;
        Event event;
        event.at = clock_.now();
        event.kind = kind;
        event.node = node;
        event_sink_.record(event);
    }

  public:
    // Returns nullptr when the slot is empty. A pointer rather than a checked reference because
    // this header compiles into the caller: an assert would vanish under NDEBUG and leave a
    // disengaged optional being dereferenced, and throwing inside __wrap_malloc is not an option.
    Injector* injector_or_null() { return injector_ ? &*injector_ : nullptr; }
    const Injector* injector_or_null() const { return injector_ ? &*injector_ : nullptr; }

    template <typename... Args>
        requires(!HasFactory<Injector>)
    Injector& emplace_injector(Args&&... args) {
        return injector_.emplace(std::forward<Args>(args)...);
    }

    void clear_injector()
        requires(!HasFactory<Injector>)
    {
        // Heal first: an active episode's heal deadline would otherwise outlive its injector.
        if constexpr (requires { injector_->heal_all(); }) {
            if (injector_.has_value()) injector_->heal_all();
        }
        injector_.reset();
    }

  private:
    // The episode layer reaches the runtime through these, never through a Simulator global.
    EpisodeDeps episode_deps() {
        EpisodeDeps deps;
        deps.timers = &scheduler_;
        deps.nodes = &nodes_;
        deps.context = this;
        deps.crash = &BasicSimulator::episode_crash;
        deps.reboot = &BasicSimulator::episode_reboot;
        deps.partition = &BasicSimulator::episode_partition;
        deps.heal_partition = &BasicSimulator::episode_heal_partition;
        deps.pause = &BasicSimulator::episode_pause;
        deps.resume = &BasicSimulator::episode_resume;
        return deps;
    }

    static void episode_crash(void* context, NodeId node) {
        (void)static_cast<BasicSimulator*>(context)->crash_node(node);
    }

    static void episode_reboot(void* context, NodeId node) {
        (void)static_cast<BasicSimulator*>(context)->reboot_node(node);
    }

    static void episode_partition(void* context, const NodeSet& a, const NodeSet& b) {
        static_cast<BasicSimulator*>(context)->net_.partition(a, b);
    }

    static void episode_heal_partition(void* context, const NodeSet& a, const NodeSet& b) {
        static_cast<BasicSimulator*>(context)->net_.heal_partition(a, b);
    }

    static void episode_pause(void* context, NodeId node) {
        static_cast<BasicSimulator*>(context)->scheduler_.pause_node(node);
    }

    static void episode_resume(void* context, NodeId node) {
        static_cast<BasicSimulator*>(context)->scheduler_.resume_node(node);
    }

    // thread_local on purpose: a universe belongs to one OS thread. Fibers of
    // one universe share this slot; wrapped calls on any fiber reach the same
    // universe. A second OS thread gets its own empty slot and falls through
    // to the host until fibers span threads (out of scope for v1).
    inline static thread_local BasicSimulator* current_sim_{nullptr};
    uint64_t seed_;
    TrackedHeap heap_{};
    Rng user_rng_;
    Rng swarm_rng_;
    Rng workload_rng_;
    VirtualClock clock_{};
    // After clock_: borrows it for timer wakeups. Before injector_: both borrow
    // the clock and destruction is reverse order.
    Scheduler scheduler_;
    // After scheduler_ and clock_, both of which it borrows.
    Net net_;
    // Declared before the injector so they outlive it; neither borrows it.
    NodeRegistry nodes_{};
    StorageModel storage_{};
    // RawRealAllocator: configuring stdin is harness work, not an application allocation.
    struct StdinIdentity {
        bool valid = false;
        dev_t dev = 0;
        ino_t ino = 0;
        mode_t mode = 0;
    };

    std::vector<char, RawRealAllocator<char>> stdin_{};
    std::size_t stdin_offset_{0};
    bool stdin_owned_{false};
    StdinIdentity stdin_identity_{};
    // Declared after clock_ on purpose: the injector borrows it, and destruction is reverse order.
    std::optional<Injector> injector_{};
    TraceOptions default_trace_options_{};
    // Declared last: every producer above borrows it only through enable_event_capture(), which
    // runs before the universe is current.
    EventSink event_sink_{};
    Telemetry telemetry_{};
    std::vector<RawString, RawRealAllocator<RawString>> application_labels_{};
    bool sink_attached_{false};
};

using Simulator = BasicSimulator<BasicFaultInjector<VirtualClock>>;

} // namespace cosmos

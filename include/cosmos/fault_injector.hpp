#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/ledger.hpp"
#include "cosmos/node_registry.hpp"
#include "cosmos/random.hpp"
#include "cosmos/task.hpp"
#include "cosmos/time.hpp"
#include "cosmos/trace.hpp"
#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace cosmos {

// P6 swaps VirtualClock's implementation, not this parameter: the injector only ever reads now().
template <typename C>
concept ClockLike = requires(const C& clock) {
    { clock.now() } -> std::same_as<Time>;
};

// Runtime hooks an episode needs; function pointers keep the injector free of Simulator/Net.
struct EpisodeDeps {
    Scheduler* timers = nullptr;
    const NodeRegistry* nodes = nullptr;
    void* context = nullptr;
    void (*crash)(void*, NodeId) = nullptr;
    void (*reboot)(void*, NodeId) = nullptr;
    void (*partition)(void*, const NodeSet&, const NodeSet&) = nullptr;
    void (*heal_partition)(void*, const NodeSet&, const NodeSet&) = nullptr;
    void (*pause)(void*, NodeId) = nullptr;
    void (*resume)(void*, NodeId) = nullptr;
};

// Pure so the edges can be pinned at values no seed reaches; rate 0 returns before dividing.
constexpr FaultKind select_outcome(double value, const FaultRule& rule) {
    if (value >= rule.rate) return FaultKind::None;

    const double rescaled = value / rule.rate;
    double cumulative = 0.0;
    for (const SiteOutcome& outcome : rule.outcomes) {
        cumulative += outcome.weight;
        if (rescaled < cumulative) return outcome.kind;
    }
    // Normalized weights can sum to a hair under 1, leaving a draw past the last bucket.
    return FaultKind::None;
}

// Rate 0 cannot fire so it never draws (Rule 3); rate 1 still draws, to pick the outcome.
constexpr bool consumes_draw(const FaultRule& rule) { return rule.rate > 0.0; }

template <ClockLike Clock> class BasicFaultInjector {
  public:
    // A factory, not a constructor: an assert would wave an invalid config through under NDEBUG.
    [[nodiscard]] static std::expected<BasicFaultInjector, ConfigProblem>
    create(FaultConfig cfg, uint64_t fault_stream_seed, uint32_t node_count, const Clock& clock,
           TraceOptions trace_options = {}, EpisodeDeps deps = {}) {
        if (const auto checked = cfg.validate(node_count); !checked.has_value()) {
            return std::unexpected(checked.error());
        }
        // An enabled trace with no room is a configuration mistake, not a silently empty trace.
        if (trace_options.enabled && trace_options.capacity == 0) {
            return std::unexpected(ConfigProblem{ConfigError::BadTraceCapacity, std::nullopt});
        }
        return BasicFaultInjector(std::move(cfg), fault_stream_seed, clock, trace_options, deps);
    }

    // A copy would fork the engine: two stream positions and two budgets against one run.
    BasicFaultInjector(const BasicFaultInjector&) = delete;
    BasicFaultInjector& operator=(const BasicFaultInjector&) = delete;
    BasicFaultInjector(BasicFaultInjector&&) = default;
    BasicFaultInjector& operator=(BasicFaultInjector&&) = delete;

    // Gate order is the contract: each early return is what keeps a dead fault from shifting an
    // unrelated class's stream (§10, Rule 3). cls is a cross-check only; the site names the class.
    // Wrapper sites only: the wrapper has already deemed the call eligible (Rule 3).
    FaultKind decide(FaultClass cls, SiteId site) {
        assert(!is_event_site(site));
        return decide_at(cls, site);
    }

    // Event sites have no wrapper call; the harness invokes them (crashes, torn writes, episodes).
    FaultKind decide_event(FaultClass cls, SiteId site) {
        assert(is_event_site(site));
        return decide_at(cls, site);
    }

    // ---- Episodes (§10.2): start with a limit check, always schedule a heal ----
    static constexpr size_t kMaxEpisodes = 32;
    std::optional<EpisodeId> start_partition(const NodeSet& a, const NodeSet& b,
                                             Duration until_heal) {
        return start_episode(SiteId::partition, 0, a, b, until_heal);
    }
    std::optional<EpisodeId> crash_node(NodeId node, Duration until_reboot) {
        return start_episode(SiteId::crash_node, node, {}, {}, until_reboot);
    }
    std::optional<EpisodeId> pause_node(NodeId node, Duration until_resume) {
        return start_episode(SiteId::pause_node, node, {}, {}, until_resume);
    }
    // Persistent crash: deliberately no heal; only quiesce clears it (§4.2.1).
    std::optional<EpisodeId> abandon_node(NodeId node) {
        return start_episode(SiteId::crash_node, node, {}, {}, Duration{1}, true);
    }
    // One-way: heal everything touching `core`, make the rest persistent, and keep it that way.
    bool liveness() const { return liveness_; }
    void heal(EpisodeId id) {
        if (id == 0) return;
        for (ActiveEpisode& episode : active_) {
            if (episode.used && episode.id == id) {
                heal_episode(episode);
                return;
            }
        }
    }
    void heal_all() {
        for (ActiveEpisode& episode : active_) {
            if (episode.used) heal_episode(episode);
        }
    }
    // Terminal settle-down: faults off, then every episode (persistent included) healed.
    void begin_quiesce() {
        quiescing_ = true;
        heal_all();
    }
    bool enter_liveness_mode(const NodeSet& core) {
        if (liveness_) return false;
        liveness_ = true;
        core_ = core;
        for (ActiveEpisode& episode : active_) {
            if (!episode.used) continue;
            if (touches_core(episode.site, episode.node, episode.a, episode.b)) {
                heal_episode(episode);
            } else {
                make_persistent(episode);
            }
        }
        return true;
    }
    bool quiescing() const { return quiescing_; }
    bool faults_quiet() const { return quiet_depth_ > 0 || quiescing_; }
    int quiet_depth() const { return quiet_depth_; }
    size_t active_episodes() const { return active_count_; }
    bool arm_scheduled() {
        if (cfg_.scheduled_episodes.size() > kMaxScheduledEpisodes) return false;
        for (size_t i = 0; i < cfg_.scheduled_episodes.size(); ++i) {
            ScheduledEvent& event = scheduled_[i];
            event.at = cfg_.scheduled_episodes[i].at;
            event.spec = cfg_.scheduled_episodes[i].spec;
            event.until = cfg_.scheduled_episodes[i].until_heal;
            event.owner = this;
            event.deadline =
                deps_.timers->schedule_at(event.at, &BasicFaultInjector::fire_scheduled, &event);
            if (event.deadline == 0) {
                disarm_scheduled();
                return false;
            }
            event.used = true;
        }
        return true;
    }

    // A counter, not a flag: an inner critical section ending must not re-enable the outer one.
    void push_quiet() { ++quiet_depth_; }

    // Clamped as well as asserted: underflow re-enables faults inside a live critical section.
    void pop_quiet() {
        assert(quiet_depth_ > 0);
        if (quiet_depth_ > 0) --quiet_depth_;
    }

    // Outcome weights read back normalized to sum to 1, not as the caller wrote them.
    const FaultConfig& config() const { return cfg_; }

    uint64_t eligible_calls(SiteId site) const {
        const size_t slot = site_slot(site);
        return slot == kNoSite ? 0 : eligible_calls_[slot];
    }

    uint64_t injections(SiteId site) const {
        const size_t slot = site_slot(site);
        return slot == kNoSite ? 0 : injections_[slot];
    }

    const FaultLedger& ledger() const { return ledger_; }

    const FaultTrace& trace() const { return trace_; }
    uint64_t decisions() const { return decisions_; }

    // Every draw in the engine goes through one place, so this counts the stream's exact advance.
    uint64_t draws() const { return draws_; }

    // §11.2's replay input: honour this schedule, withholding whatever the filter names. The
    // caller keeps the trace alive; the injector only reads it.
    void replay_from(const FaultTrace& source, TraceFilter filter) {
        source_ = &source;
        filter_ = std::move(filter);
        cursor_ = 0;
    }

    bool replaying() const { return source_ != nullptr; }
    uint64_t replay_consumed() const { return cursor_; }
    uint64_t replay_withheld() const { return withheld_; }

    // The universe names its own trace, so a replay cannot be seeded against the wrong run.
    void set_universe_seed(uint64_t seed) { trace_.set_universe_seed(seed); }

  private:
    BasicFaultInjector(FaultConfig cfg, uint64_t fault_stream_seed, const Clock& clock,
                       TraceOptions trace_options, EpisodeDeps deps)
        : cfg_(std::move(cfg)),
          streams_(make_streams(fault_stream_seed, std::make_index_sequence<kFaultClassCount>{})),
          clock_(clock),
          // Sized once, while no universe is current: the buffer is not tracked and not faulted.
          trace_(trace_options.enabled ? trace_options.capacity : 0), deps_(deps) {
        cfg_.normalize();
    }

    template <size_t... I>
    static std::array<Rng, sizeof...(I)> make_streams(uint64_t seed, std::index_sequence<I...>) {
        return {Rng(fault_class_seed(seed, static_cast<FaultClass>(I)))...};
    }

    // Shared gate chain: wrapper calls and harness-driven event sites differ only in who calls it.
    FaultKind decide_at(FaultClass cls, SiteId site) {
        assert(class_of(site) == cls);

        const size_t slot = site_slot(site);
        if (slot == kNoSite) return FaultKind::None;

        if (quiet_depth_ > 0 || quiescing_) return FaultKind::None;
        if (clock_.now() >= cfg_.quiesce_after) return FaultKind::None;

        // From the site, never the argument: gating on one class and drawing from another's
        // stream would survive NDEBUG.
        const FaultClass site_class = class_of(site);
        if (!cfg_.is_class_enabled(site_class)) return FaultKind::None;
        if (!cfg_.is_site_activated(site)) return FaultKind::None;
        if (clock_.now() < cfg_.warmup_until) return FaultKind::None;

        // skip_first and the trigger count from here, so this precedes the gates below (§10).
        ++eligible_calls_[slot];

        // The swarm can activate a site without giving it a rule.
        const FaultRule* rule = cfg_.rule_for(site);
        if (rule == nullptr) return FaultKind::None;

        if (eligible_calls_[slot] <= rule->skip_first) return FaultKind::None;
        if (injections_[slot] >= rule->max_injections) return FaultKind::None;

        if (replaying()) return replay_decide(site_class, site, slot, *rule);

        // Entry 0 is the walk's zero point: every weight is positive, so a draw approaching 0
        // always lands there. Reading it directly is what keeps the fire zero-draw (Rule 11) —
        // passing 0.0 to select_outcome() would return None for the legal rate-0 trigger-only rule.
        if (rule->fire_on_eligible_call == eligible_calls_[slot]) {
            const FaultKind kind = rule->outcomes.entries[0].kind;
            record_decision(site, slot, *rule, kind, false);
            return record_fire(site_class, site, slot, kind, false);
        }

        const FaultKind kind = draw(site_class, *rule);
        record_decision(site, slot, *rule, kind, consumes_draw(*rule));
        if (kind == FaultKind::None) return FaultKind::None;
        return record_fire(site_class, site, slot, kind, true);
    }

    // One active episode; the array slot doubles as the heal callback context.
    struct ActiveEpisode {
        EpisodeId id = 0;
        SiteId site = SiteId::crash_node;
        NodeId node = 0;
        NodeSet a;
        NodeSet b;
        uint64_t deadline = 0;
        bool used = false;
        bool persistent = false;
        BasicFaultInjector* owner = nullptr;
    };

    static void heal_due(void* context) {
        auto* episode = static_cast<ActiveEpisode*>(context);
        if (episode->used && episode->owner != nullptr) episode->owner->heal(episode->id);
    }

    // A refusal is recorded, never silent: the ledger names the promise that would have broken.
    bool refuse(SiteId site, SkipReason reason) {
        ledger_.record(LedgerEntry{.fire_index = fires_,
                                   .at = clock_.now(),
                                   .site = site,
                                   .eligible_index = active_count_,
                                   .fault_class = class_of(site),
                                   .outcome = FaultKind::None,
                                   .drew = false,
                                   .status = DecisionStatus::Skipped,
                                   .reason = reason});
        return false;
    }

    // In replay, consume this site's recorded start; false when the filter withholds it.
    bool episode_allowed(SiteId site) {
        if (!replaying()) return true;
        const TraceEntry* entry = peek();
        if (entry == nullptr || entry->site != site) return true;
        ++cursor_;
        if (entry->status != DecisionStatus::Fired || filter_.withholds(entry->seq)) {
            ledger_.record(LedgerEntry{.fire_index = fires_,
                                       .at = clock_.now(),
                                       .site = site,
                                       .eligible_index = active_count_ + 1,
                                       .fault_class = class_of(site),
                                       .outcome = FaultKind::None,
                                       .drew = false,
                                       .status = DecisionStatus::Suppressed});
            ++withheld_;
            return false;
        }
        return true;
    }

    void record_episode(SiteId site, Duration until) {
        ++decisions_;
        if (!trace_.enabled()) return;
        trace_.record(TraceEntry{.seq = decisions_,
                                 .eligible_index = active_count_,
                                 .at = clock_.now(),
                                 .site = site,
                                 .amount = until,
                                 .outcome = FaultKind::None,
                                 .drew = false,
                                 .status = DecisionStatus::Fired});
    }

    void heal_episode(ActiveEpisode& episode) {
        if (episode.deadline != 0) (void)deps_.timers->cancel_deadline(episode.deadline);
        if (episode.site == SiteId::crash_node) {
            deps_.reboot(deps_.context, episode.node);
        } else if (episode.site == SiteId::pause_node) {
            deps_.resume(deps_.context, episode.node);
        } else {
            deps_.heal_partition(deps_.context, episode.a, episode.b);
        }
        // A heal row uses the fire counter as its ledger sequence; it is not itself a fire.
        ledger_.record(LedgerEntry{.fire_index = fires_,
                                   .at = clock_.now(),
                                   .site = episode.site,
                                   .eligible_index = active_count_,
                                   .fault_class = class_of(episode.site),
                                   .outcome = FaultKind::None,
                                   .drew = false,
                                   .persistent = episode.persistent});
        ++fires_;
        episode.used = false;
        episode.persistent = false;
        episode.deadline = 0;
        episode.a.clear();
        episode.b.clear();
        --active_count_;
    }

    bool in_core(NodeId node) const {
        for (NodeId id : core_) {
            if (id == node) return true;
        }
        return false;
    }

    // A fault touches core if any node it names is in the core set.
    bool touches_core(SiteId site, NodeId node, const NodeSet& a, const NodeSet& b) const {
        if (site != SiteId::partition) return in_core(node);
        for (NodeId id : a) {
            if (in_core(id)) return true;
        }
        for (NodeId id : b) {
            if (in_core(id)) return true;
        }
        return false;
    }

    void make_persistent(ActiveEpisode& episode) {
        if (episode.deadline != 0) (void)deps_.timers->cancel_deadline(episode.deadline);
        episode.deadline = 0;
        episode.persistent = true;
    }

    struct ScheduledEvent {
        Time at{};
        EpisodeSpec spec{};
        Duration until{};
        uint64_t deadline = 0;
        bool used = false;
        BasicFaultInjector* owner = nullptr;
    };

    static void fire_scheduled(void* context) {
        auto* event = static_cast<ScheduledEvent*>(context);
        if (!event->used || event->owner == nullptr) return;
        event->used = false;
        event->deadline = 0;
        event->owner->start_scheduled(*event);
    }

    // Withdraws every armed event; a half-armed install must leave no callback behind.
    void disarm_scheduled() {
        for (ScheduledEvent& event : scheduled_) {
            if (event.deadline != 0 && deps_.timers != nullptr) {
                (void)deps_.timers->cancel_deadline(event.deadline);
            }
            event.deadline = 0;
            event.used = false;
        }
    }

    void start_scheduled(ScheduledEvent& event) {
        if (const auto* crash = std::get_if<CrashNode>(&event.spec)) {
            (void)crash_node(crash->id, event.until);
        } else if (const auto* pause = std::get_if<PauseNode>(&event.spec)) {
            (void)start_episode(SiteId::pause_node, pause->id, {}, {}, event.until);
        } else if (const auto* split = std::get_if<Partition>(&event.spec)) {
            (void)start_partition(split->a, split->b, event.until);
        }
    }

    std::optional<EpisodeId> start_episode(SiteId site, NodeId node, NodeSet a, NodeSet b,
                                           Duration until, bool persistent = false) {
        if (deps_.timers == nullptr || deps_.nodes == nullptr || deps_.crash == nullptr ||
            deps_.reboot == nullptr || deps_.partition == nullptr ||
            deps_.heal_partition == nullptr || deps_.pause == nullptr || deps_.resume == nullptr) {
            (void)refuse(site, SkipReason::NoDeps);
            return std::nullopt;
        }
        if (quiescing_ || clock_.now() >= cfg_.quiesce_after) {
            (void)refuse(site, SkipReason::Quiesce);
            return std::nullopt;
        }
        if (clock_.now() < cfg_.warmup_until) {
            (void)refuse(site, SkipReason::Warmup);
            return std::nullopt;
        }
        if (!cfg_.is_site_activated(site) || !cfg_.is_class_enabled(class_of(site))) {
            (void)refuse(site, SkipReason::SiteDisabled);
            return std::nullopt;
        }
        if (until <= Duration::zero()) {
            (void)refuse(site, SkipReason::BadDuration);
            return std::nullopt;
        }
        if (site == SiteId::crash_node || site == SiteId::pause_node) {
            if (!deps_.nodes->is_known(node)) {
                (void)refuse(site, SkipReason::UnknownNode);
                return std::nullopt;
            }
            if (!deps_.nodes->is_live(node)) {
                (void)refuse(site, SkipReason::AlreadyCrashed);
                return std::nullopt;
            }
            // Crash limits do not apply to a pause: the node is unhealthy, not dead.
            if (site == SiteId::crash_node) {
                // Quorum first: both limits are true at the last allowed crash.
                if (deps_.nodes->live_count() <= cfg_.min_healthy_quorum) {
                    (void)refuse(site, SkipReason::MinHealthyQuorum);
                    return std::nullopt;
                }
                if (deps_.nodes->crashed_count() >= cfg_.max_crashed_nodes) {
                    (void)refuse(site, SkipReason::MaxCrashedNodes);
                    return std::nullopt;
                }
            }
        } else {
            if (a.empty() || b.empty()) {
                (void)refuse(site, SkipReason::BadPartition);
                return std::nullopt;
            }
            for (NodeId id : a) {
                if (!deps_.nodes->is_known(id)) {
                    (void)refuse(site, SkipReason::UnknownNode);
                    return std::nullopt;
                }
            }
            for (NodeId id : b) {
                if (!deps_.nodes->is_known(id)) {
                    (void)refuse(site, SkipReason::UnknownNode);
                    return std::nullopt;
                }
            }
        }

        // One node-fault at a time, after the state checks so a crashed node reports that first.
        if (site != SiteId::partition) {
            for (const ActiveEpisode& active : active_) {
                if (active.used && active.node == node) {
                    (void)refuse(site, SkipReason::NodeBusy);
                    return std::nullopt;
                }
            }
        }

        size_t free_slot = kMaxEpisodes;
        for (size_t i = 0; i < kMaxEpisodes; ++i) {
            if (!active_[i].used) {
                free_slot = i;
                break;
            }
        }
        if (free_slot == kMaxEpisodes) {
            (void)refuse(site, SkipReason::TooManyEpisodes);
            return std::nullopt;
        }
        if (!episode_allowed(site)) return std::nullopt;

        // After the liveness transition, a fault touching no core node stays broken by design.
        const bool forever = persistent || (liveness_ && !touches_core(site, node, a, b));
        ActiveEpisode& episode = active_[free_slot];
        episode.id = next_episode_++;
        episode.site = site;
        episode.node = node;
        episode.a = std::move(a);
        episode.b = std::move(b);
        episode.persistent = forever;
        episode.owner = this;
        if (!forever) {
            episode.deadline = deps_.timers->schedule_at(clock_.now() + until,
                                                         &BasicFaultInjector::heal_due, &episode);
            if (episode.deadline == 0) {
                (void)refuse(site, SkipReason::TooManyEpisodes);
                return std::nullopt;
            }
        }
        episode.used = true;
        ++active_count_;
        if (site == SiteId::crash_node) {
            deps_.crash(deps_.context, node);
        } else if (site == SiteId::pause_node) {
            deps_.pause(deps_.context, node);
        } else {
            deps_.partition(deps_.context, episode.a, episode.b);
        }
        ledger_.record(LedgerEntry{.fire_index = fires_,
                                   .at = clock_.now(),
                                   .site = site,
                                   .eligible_index = active_count_,
                                   .fault_class = class_of(site),
                                   .outcome = FaultKind::None,
                                   .drew = false,
                                   .persistent = forever});
        ++fires_;
        record_episode(site, forever ? Duration::zero() : until);
        return episode.id;
    }

    // Rate 0 cannot fire so it never draws (Rule 3); rate 1 still draws, to pick the outcome.
    FaultKind draw(FaultClass cls, const FaultRule& rule) {
        if (!consumes_draw(rule)) return FaultKind::None;
        return select_outcome(next_value(cls), rule);
    }

    // The one place a class stream advances, so draws() and the stream cannot disagree.
    double next_value(FaultClass cls) {
        ++draws_;
        return streams_[static_cast<size_t>(cls)].uniform();
    }

    const TraceEntry* peek() const {
        if (source_ == nullptr || cursor_ >= source_->size()) return nullptr;
        return &(*source_)[cursor_];
    }

    // §11.2: one cursor. A site match consumes the entry; anything else passes through untouched,
    // because a replay never invents a fault the source run did not have (Rule 12).
    FaultKind replay_decide(FaultClass cls, SiteId site, size_t slot, const FaultRule& rule) {
        const TraceEntry* entry = peek();
        if (entry == nullptr || entry->site != site) {
            record_decision(site, slot, rule, FaultKind::None, false);
            return FaultKind::None;
        }

        ++cursor_;
        // Consumed even when withheld: the stream has to end where the source run left it.
        if (entry->drew) (void)next_value(cls);

        // A trace that is itself a replay already carries its suppressions; never re-fire them.
        const bool suppressed =
            entry->status != DecisionStatus::Fired || filter_.withholds(entry->seq);
        if (suppressed) {
            record_withheld(cls, site, slot, *entry);
            record_decision(site, slot, rule, entry->outcome, entry->drew,
                            DecisionStatus::Suppressed);
            return FaultKind::None;
        }

        record_decision(site, slot, rule, entry->outcome, entry->drew);
        if (entry->outcome == FaultKind::None) return FaultKind::None;
        return record_fire(cls, site, slot, entry->outcome, entry->drew);
    }

    // §11.1 shows what the application was spared, so a withheld fire is a ledger row of its own.
    // It takes no fire_index — nothing fired, so the field names the index it would have had.
    void record_withheld(FaultClass cls, SiteId site, size_t slot, const TraceEntry& entry) {
        ++withheld_;
        ledger_.record(LedgerEntry{.fire_index = fires_,
                                   .at = clock_.now(),
                                   .site = site,
                                   .eligible_index = eligible_calls_[slot],
                                   .fault_class = cls,
                                   .outcome = entry.outcome,
                                   .drew = entry.drew,
                                   .status = DecisionStatus::Suppressed});
    }

    // One entry per post-gate decision, numbered by the trace's own seq, never by fire_index. The
    // counter runs whether or not the trace does, so decisions() stays the decision count.
    void record_decision(SiteId site, size_t slot, const FaultRule& rule, FaultKind kind, bool drew,
                         DecisionStatus status = DecisionStatus::Fired) {
        ++decisions_;
        if (!trace_.enabled()) return;
        trace_.record(TraceEntry{.seq = decisions_,
                                 .eligible_index = eligible_calls_[slot],
                                 .at = clock_.now(),
                                 .site = site,
                                 .amount = rule.amount,
                                 .outcome = kind,
                                 .drew = drew,
                                 .status = status});
    }

    // Both fire paths land here, so the counter and the ledger cannot drift apart.
    FaultKind record_fire(FaultClass cls, SiteId site, size_t slot, FaultKind kind, bool drew) {
        ++injections_[slot];
        ledger_.record(LedgerEntry{.fire_index = fires_,
                                   .at = clock_.now(),
                                   .site = site,
                                   .eligible_index = eligible_calls_[slot],
                                   .fault_class = cls,
                                   .outcome = kind,
                                   .drew = drew});
        ++fires_;
        return kind;
    }

    FaultConfig cfg_;
    std::array<Rng, kFaultClassCount> streams_;
    const Clock& clock_; // borrowed; the caller keeps it alive for the injector's whole life
    int quiet_depth_{0};
    SiteCounterMap eligible_calls_{};
    SiteCounterMap injections_{};
    FaultLedger ledger_{};
    uint64_t fires_{0};
    FaultTrace trace_{};
    uint64_t decisions_{0};
    uint64_t draws_{0};
    const FaultTrace* source_{nullptr};
    TraceFilter filter_{};
    size_t cursor_{0};
    uint64_t withheld_{0};
    std::array<ActiveEpisode, kMaxEpisodes> active_{};
    std::array<ScheduledEvent, kMaxScheduledEpisodes> scheduled_{};
    bool liveness_{false};
    NodeSet core_;
    size_t active_count_{0};
    EpisodeId next_episode_{1};
    bool quiescing_{false};
    EpisodeDeps deps_{};
};

// Non-movable too: a second owner would pop a quiet window it never pushed.
template <typename Injector> class QuietGuard {
  public:
    explicit QuietGuard(Injector& injector) : injector_(injector) { injector_.push_quiet(); }
    ~QuietGuard() { injector_.pop_quiet(); }

    QuietGuard(const QuietGuard&) = delete;
    QuietGuard& operator=(const QuietGuard&) = delete;
    QuietGuard(QuietGuard&&) = delete;
    QuietGuard& operator=(QuietGuard&&) = delete;

  private:
    Injector& injector_;
};

} // namespace cosmos

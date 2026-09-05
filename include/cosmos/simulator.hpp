#pragma once

#include "cosmos/fault_injector.hpp"
#include "cosmos/faults.hpp"
#include "cosmos/memory.hpp"
#include "cosmos/random.hpp"
#include "cosmos/time.hpp"
#include <expected>
#include <optional>
#include <utility>

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
        : seed_(seed), user_rng_(stream_seed(seed, StreamDomain::User)) {}

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

    VirtualClock& clock() { return clock_; }
    const VirtualClock& clock() const { return clock_; }

    Time now() const { return clock_.now(); }
    void advance_time(Duration d) { clock_.advance(d); }

    uint64_t seed() const { return seed_; }

    [[nodiscard]] std::expected<void, ConfigProblem> install_faults(FaultConfig cfg,
                                                                    uint32_t node_count = 1) {
        // A second install would fork stream positions, counters and the ledger against one run.
        if (injector_.has_value()) {
            return std::unexpected(
                ConfigProblem{ConfigError::InjectorAlreadyInstalled, std::nullopt});
        }
        // Seeding from the universe seed instead of the Fault domain would collide five of six
        // class sub-streams with the domain streams.
        auto made = Injector::create(std::move(cfg), stream_seed(seed_, StreamDomain::Fault),
                                     node_count, clock_);
        if (!made.has_value()) return std::unexpected(made.error());
        injector_.emplace(std::move(*made));
        return {};
    }

    // User-visible randomness (getrandom/random values). Never draws for fault decisions (Rule 1).
    Rng& user_rng() { return user_rng_; }
    const Rng& user_rng() const { return user_rng_; }

    bool has_injector() const { return injector_.has_value(); }

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
        injector_.reset();
    }

  private:
    // thread_local on purpose: a universe belongs to one OS thread. Caveat until the fiber
    // scheduler lands and pthread_create is genuinely wrapped: threads spawned through the
    // current passthrough get their own empty slot on their own OS thread, so wrapped calls
    // made there fall through to the real host clock/heap instead of this universe. Keep
    // simulation workloads single-threaded until then, or real and virtual state will mix.
    inline static thread_local BasicSimulator* current_sim_{nullptr};
    uint64_t seed_;
    TrackedHeap heap_{};
    Rng user_rng_;
    VirtualClock clock_{};
    // Declared after clock_ on purpose: the injector borrows it, and destruction is reverse order.
    std::optional<Injector> injector_{};
};

using Simulator = BasicSimulator<BasicFaultInjector<VirtualClock>>;

} // namespace cosmos

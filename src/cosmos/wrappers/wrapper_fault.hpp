#pragma once

// Internal helpers shared by the __wrap_* translation units. Deliberately not part of the
// public include/ surface, and header-only so every __wrap_* function stays in its own
// -ffunction-sections section: --gc-sections must still be able to discard an unused wrapper
// without dragging in the others (docs/design.md §2).

#include "cosmos/faults.hpp"

#include <cstddef>

namespace cosmos::wrappers {

// Set for the duration of one wrapper's own logic. A wrapped POSIX call arriving while this is
// set is a re-entry from engine-internal work: it passes through to the real call without a
// fault decision, mirroring wrap_memory.cpp's in_wrap_memory guard. The previous value is
// restored rather than cleared, so a guard instantiated in an inner scope cannot unguard an
// outer one.
inline thread_local bool in_wrapper_logic = false;

struct ReentrancyGuard {
    ReentrancyGuard() : previous_(in_wrapper_logic) { in_wrapper_logic = true; }
    ~ReentrancyGuard() { in_wrapper_logic = previous_; }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;

  private:
    bool previous_;
};

// The one fault-decision hook for wrapper translation units. Compiles against the current
// NoInjector placeholder (folds to None) and forwards to FaultInjector::decide once the
// Simulator alias carries the real engine, so wrapper files do not change when the wiring
// lands. Exactly one call per eligible wrapped call, zero calls on passthrough and no-op
// paths — the gate-before-draw discipline of fault-injection.md Rule 3, enforced at the
// call site by construction. A null simulator means no universe is running: no decision.
template <typename Sim> FaultKind decide_for(Sim* sim, FaultClass cls, SiteId site) {
    if (sim == nullptr) {
        return FaultKind::None;
    }
    if constexpr (requires { sim->injector_or_null()->decide(cls, site); }) {
        if (auto* injector = sim->injector_or_null()) {
            return injector->decide(cls, site);
        }
    }
    return FaultKind::None;
}

// Eligibility policy for storage I/O: a call reaches the injector only when a decision could
// produce a legal, observable outcome on it (Rule 15 applied to eligibility). Compile-time so
// the policy is pinned and testable without an injector (tests/test_wrap_storage.cpp).
//
// - Standard stream fds (0/1/2) are never eligible: application and harness logging must not
//   consume Storage-stream draws or fail with injected errors — terminal/pipe writes are not
//   the disk fault model. Every other fd, negative ones included, goes to the host untouched
//   by the decision (a negative fd is an EBADF from the host, not a storage fault).
// - Zero-length reads and writes are never eligible: no outcome is observable.
// - 1-byte writes ARE eligible: they can legally fail with EIO/ENOSPC (a single-byte WAL
//   commit marker is a real shape). Only the ShortWrite outcome is unobservable there, which
//   the wrapper degrades per-call (see wrap_storage.cpp).
inline constexpr bool storage_fd_eligible(int fd) { return fd > 2; }

inline constexpr bool storage_read_eligible(int fd, size_t count) {
    return storage_fd_eligible(fd) && count > 0;
}

inline constexpr bool storage_write_eligible(int fd, size_t count) {
    return storage_fd_eligible(fd) && count > 0;
}

} // namespace cosmos::wrappers

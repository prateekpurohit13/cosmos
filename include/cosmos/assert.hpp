#pragma once

// Application-facing property checks (docs/design.md §12). They report into the thread's current
// Scenario, so an app never has to thread a harness object through its own code.

#include "cosmos/scenario.hpp"
#include <cassert>
#include <string>
#include <utility>

namespace cosmos {

// An invariant of this universe: a violation is recorded at once, a pass is silent.
// Valid inside Scenario::run(), Scenario::check() and the quiesce drain; use Scenario::check()
// for anything that must be judged after the world has settled.
inline void always(bool cond, std::string id, std::string detail = "") {
    if (cond) return;
    Scenario* scenario = Scenario::current();
    // assert-then-return rather than a checked reference: this header compiles into the caller.
    assert(scenario != nullptr && "cosmos::always() called outside a Scenario");
    if (scenario != nullptr) scenario->record_violation(std::move(id), std::move(detail));
}

// A liveness property that must hold in at least one universe: the ids no universe reports are the
// campaign's never_hit list.
inline void sometimes(bool cond, std::string id) {
    Scenario* scenario = Scenario::current();
    assert(scenario != nullptr && "cosmos::sometimes() called outside a Scenario");
    if (scenario != nullptr) scenario->record_sometimes(std::move(id), cond);
}

// Marks a path as exercised, without naming a condition.
inline void reachable(std::string id) { sometimes(true, std::move(id)); }

} // namespace cosmos

// The call site is the detail, so an unnamed check still says where it failed.
#define COSMOS_CHECK(cond, id)                                                                     \
    ::cosmos::always((cond), (id), std::string(__FILE__) + ":" + std::to_string(__LINE__))

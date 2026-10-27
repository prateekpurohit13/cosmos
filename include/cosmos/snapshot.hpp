#pragma once

// Snapshot branching for level-3 exploration (docs/state-exploration.md §6; docs/architecture.md
// §5–§6). The design permits two implementations: an in-process stack/heap copy, or a fork-COW
// checkpoint. Cosmos uses the fork-COW form because the tracked heap is not address-stable and a
// restoring copy would have to recreate allocations at their original addresses. A forked
// checkpoint captures the heap, every fiber stack and register set, the scheduler queues, the
// network queues, the RNGs and the fault injector as one COW image. Host storage is the one thing
// fork does not isolate, so capture copies tracked files and every branch restores them first.
//
// A Snapshot is a live checkpoint process, not a serialized object. `explore()` asks that process
// for a branch child; the child runs the continuation body and returns a ScenarioReport. This is
// the process-based analogue of restore_snapshot(), and the honest label for it is "fork-cow".
//
// Storage isolation covers the durability model's tracked descriptors and direct calls to the
// wrapped open/open64/__open_2/__open64_2 symbols. Unwrapped path APIs (fopen, openat, creat,
// libstdc++ stream internals) bypass link interposition and are outside the determinism contract.
// Capture also copies untracked regular descriptors with procfs-reachable paths into the branch
// image.
//
// The canonical trace and trace_hash are untouched: branch children use the same trace encoding,
// and their coverage bitmap is a separate observation.

#include "cosmos/coverage.hpp"
#include "cosmos/scenario.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace cosmos {

inline constexpr uint64_t kNoBranch = std::numeric_limits<uint64_t>::max();

// A point-in-time description captured in the parent before forking the checkpoint. The child owns
// the actual process image; this is what artifacts and the UI can show without claiming that a
// branch was replayed in-process.
struct SnapshotInfo {
    // Process-local identity. A caller that captures inside a branch should pass the parent id so
    // the artifact tree can distinguish it; default-id collisions across forked branches are not
    // a global namespace.
    uint64_t id = 0;
    uint64_t parent_id = 0;
    // Snapshot parent plus the branch of that snapshot in which this checkpoint was captured.
    uint64_t parent_branch_index = kNoBranch;
    uint64_t seed = 0;
    Time at{};
    uint64_t tasks = 0;
    uint64_t ready = 0;
    uint64_t blocked = 0;
    uint64_t ready_choices = 0;
    uint64_t heap_allocations = 0;
    uint64_t heap_bytes = 0;
    uint64_t endpoints = 0;
    uint64_t pending_packets = 0;
    uint64_t storage_files = 0;
    uint64_t trace_decisions = 0;
    uint64_t trace_entries = 0;
    bool coverage_present = false;
    uint64_t coverage_edges = 0;
    uint64_t coverage_hash = 0;
    bool faults_quiet = false;
    std::string method = "fork-cow";
};

// One branch requested from a checkpoint. `forced_choice` is a ready-queue index consumed by the
// next scheduler choice. `schedule_seed` reseeds the schedule stream after that choice; zero keeps
// the snapshot's stream.
struct BranchContext {
    uint64_t branch_index = 0;
    uint64_t snapshot_id = 0;
    uint64_t parent_snapshot_id = 0;
    bool has_forced_choice = false;
    uint64_t forced_choice = 0;
    bool has_schedule_seed = false;
    uint64_t schedule_seed = 0;
};

// A nested checkpoint's branch, flattened into the containing branch result. `snapshot.parent_id`
// and `snapshot.parent_branch_index` identify the checkpoint it belongs to.
struct NestedBranch {
    SnapshotInfo snapshot{};
    BranchContext context{};
    bool died = false;
    int signal = 0;
    bool forced_choice_applied = false;
    std::string threw{};
    std::string error{};
    ScenarioReport report{};
    Time ended_at{};
    uint64_t wall_ns = 0;
    bool traced = false;
    uint64_t trace_hash = 0;
    uint64_t trace_capacity = 0;
    uint64_t trace_dropped = 0;
    std::string trace_bytes{};
    bool coverage_present = false;
    std::string coverage_method{};
    uint64_t coverage_edges = 0;
    uint64_t coverage_hash = 0;
};

// The result of one branch child. It mirrors the campaign's UniverseOutcome fields the UI needs,
// without making snapshot users depend on the campaign worker channel.
struct BranchResult {
    BranchContext context{};
    SnapshotInfo snapshot{};
    ScenarioReport report{};
    bool died = false;
    int signal = 0;
    bool forced_choice_applied = false;
    std::string threw;
    std::string error;
    Time ended_at{};
    uint64_t wall_ns = 0;
    bool traced = false;
    uint64_t trace_hash = 0;
    uint64_t trace_capacity = 0;
    uint64_t trace_dropped = 0;
    // Branch trace begins at the snapshot, not at the universe seed. It is observation data for
    // the continuation and is not a canonical replay identity.
    std::string trace_bytes;
    bool coverage_present = false;
    std::string coverage_method{};
    CoverageSnapshot coverage{};
    // Flattened nested captures and their branch outcomes. The lists are populated by the branch
    // child's recorder, so a snapshot inside a branch is represented rather than silently lost.
    std::vector<SnapshotInfo> nested_snapshots{};
    std::vector<NestedBranch> nested_branches{};
};

// The continuation runs in the branch child, after the checkpoint has restored storage and before
// the result is collected. It may call scenario.drain(), mutate branch-local application state,
// and record checks; the snapshot runner calls scenario.quiesce() afterwards.
using BranchBody = std::function<void(Scenario&, const BranchContext&)>;

class Snapshot {
  public:
    // Forks a checkpoint from `scenario`'s current universe state. Capture is refused inside a
    // running fiber or an active scheduler drain; capture between workload phases. Any default
    // BranchBody drains the ready fibers, which is the time-travel continuation. `parent_id` is
    // recorded so a branch tree can be reconstructed by an artifact writer.
    static std::expected<Snapshot, std::string> capture(Scenario& scenario, BranchBody body = {},
                                                        uint64_t parent_id = 0);

    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&& other) noexcept;
    Snapshot& operator=(Snapshot&& other) noexcept;
    ~Snapshot();

    const SnapshotInfo& info() const;

    // Asks the checkpoint for one branch child and waits for its report. Calls are serialized:
    // only one branch runs at a time, so a shared host file image can be restored between them.
    BranchResult explore(const BranchContext& context);

  private:
    struct Impl;
    explicit Snapshot(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Seam B from docs/architecture.md. SimSubstrate is the fork-COW implementation; a future
// KvmSubstrate would implement the same two operations over a VM snapshot.
class ISubstrate {
  public:
    virtual ~ISubstrate() = default;
    virtual std::expected<std::unique_ptr<Snapshot>, std::string>
    save_snapshot(Scenario& scenario, BranchBody body = {}, uint64_t parent_id = 0) = 0;
    virtual BranchResult restore_snapshot(Snapshot& snapshot, const BranchContext& context) = 0;
};

class SimSubstrate final : public ISubstrate {
  public:
    std::expected<std::unique_ptr<Snapshot>, std::string>
    save_snapshot(Scenario& scenario, BranchBody body = {}, uint64_t parent_id = 0) override {
        auto captured = Snapshot::capture(scenario, std::move(body), parent_id);
        if (!captured) return std::unexpected(captured.error());
        return std::make_unique<Snapshot>(std::move(*captured));
    }

    BranchResult restore_snapshot(Snapshot& snapshot, const BranchContext& context) override {
        return snapshot.explore(context);
    }
};

} // namespace cosmos

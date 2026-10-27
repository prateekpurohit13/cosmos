#pragma once

#include "cosmos/event_sink.hpp"
#include "cosmos/faults.hpp"
#include "cosmos/time.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace cosmos {

// Read-only row for the storage state tab. Sizes are bytes, offsets are virtual file positions.
// One fd can appear under more than one file only after a rebuild; the model never shares fds.
struct FileInfo {
    uint64_t id = 0;
    std::string path{};
    NodeId node = 0;
    int64_t durable_size = 0;
    uint64_t unsynced_records = 0;
    uint64_t unsynced_bytes = 0;
    int64_t last_sync_offset = 0;
    uint64_t last_sync_length = 0;
    bool degraded = false;
    uint64_t open_fds = 0;
};

// Open descriptor state needed to make a branch use private file copies. `path` is the model's
// canonical (possibly redirected) path at capture time.
struct OpenDescriptorInfo {
    int fd = -1;
    off_t offset = 0;
    int flags = 0;
    int fd_flags = 0;
    std::string path{};
};

// Simulated durability (docs/design.md §8): a write lands in an un-synced page cache, fsync makes
// it durable, and a reboot discards whatever was not. Bytes stay in the host file so lseek and
// ftruncate keep native semantics; the model only remembers what to put back.
class StorageModel {
  public:
    // Pre-image bytes held per universe; past this the model stops simulating discard for a file.
    static constexpr size_t kMaxTrackedBytes = 32u << 20;

    // Node that later opens belong to. Single-node harnesses never set it; P6 uses it per node.
    void set_active_node(NodeId node) { active_node_ = node; }
    NodeId active_node() const { return active_node_; }

    // Registers an fd after a successful open. truncate means O_TRUNC: durability restarts empty.
    // A new file belongs to the opener; later writes re-home it (record_write). Reopening only
    // adopts a smaller size when the file shrank behind the model's back.
    void track_open(int fd, std::string path, bool truncate);
    void forget_fd(int fd);
    bool is_tracked(int fd) const { return fd_paths_.contains(fd); }

    // Copies the bytes a write is about to overwrite, so a reboot can put them back. length is
    // what the write itself transferred. `node` is the writing fiber: the last writer owns the
    // file, and a later write from another node re-attributes it (shared fds are unsupported).
    void record_write(int fd, off_t offset, std::string_view previous, size_t length, NodeId node);
    // fsync's boundary: the file's current size becomes durable and rollback records are dropped.
    void commit_fsync(int fd);
    // An application ftruncate can only lower durability: a cut drops the records past it, and a
    // growing truncate never promotes bytes that were written without fsync.
    void record_truncate(int fd, off_t new_size);

    // Discards un-synced writes for every file owned by node. torn zeroes the last synced sector,
    // because an appended sector has no older bytes to fall back to (the pre-image is empty).
    void reboot(NodeId node, bool torn);

    // True once any file outgrew kMaxTrackedBytes; discard is then a no-op for that file. Sticky
    // for the universe's lifetime: a budget that was exceeded is a fact about the run.
    bool degraded() const { return degraded_; }
    size_t tracked_bytes() const { return tracked_bytes_; }
    size_t file_count() const { return files_.size(); }

    // Read-only enumerator for the state inspector (U3.3), ordered by file id (creation order).
    std::vector<FileInfo> file_infos() const;

    // Canonical tracked paths (the keys the durability model uses). In a snapshot branch these
    // are the private copies, not the display paths returned by file_infos().
    std::vector<std::string> tracked_paths() const;

    // Open descriptors, their flags and current host offsets. Snapshot branching uses this to
    // replace each descriptor with a private branch copy, because fork shares open file
    // descriptions (and therefore offsets) even though it copies the descriptor table.
    std::vector<OpenDescriptorInfo> open_descriptors() const;

    // Branch-private path redirection. A snapshot child installs source -> private-copy mappings
    // before running a branch; open() wrappers call redirect_path(), and track_open() records the
    // private path while file_infos() displays the original source path.
    bool has_redirects() const { return branch_active_; }
    // Starts branch-private storage rooted at `root`. Every wrapped pathname is mapped: tracked
    // files use the supplied copies, and a path first seen in the branch gets a private file under
    // the root (copied from the host if it already exists). This is what keeps a new file or a
    // spelling alias from escaping to the parent.
    void begin_branch_storage(std::string root,
                              const std::map<std::string, std::string>& redirects);
    // `create` is true for O_CREAT opens: a path absent from the captured view may be created in
    // the branch, but a later branch read of a parent-created path must not expose live bytes.
    std::string redirect_path(std::string_view path, bool create = false);
    std::string canonical_path(std::string_view path) const;
    void redirect_paths(const std::map<std::string, std::string>& redirects);

    // The inspection sink is owned by the universe; a null sink disables producer recording. The
    // clock is borrowed and may be null in tests, in which case events carry virtual time zero.
    void set_event_sink(EventSink* sink, const VirtualClock* clock) {
        event_sink_ = sink;
        clock_ = clock;
    }
    EventSink* event_sink() const { return event_sink_; }

  private:
    // One write: where it started, how many bytes it wrote, and the bytes that were there.
    struct WriteRecord {
        off_t offset = 0;
        size_t length = 0;
        std::string previous;
    };

    struct FileState {
        uint64_t id = 0;
        NodeId node = 0;
        off_t durable_size = 0;
        std::vector<WriteRecord> unsynced;
        // The last range fsync committed, kept only so a torn reboot can revert its tail.
        std::optional<WriteRecord> last_sync;
        bool degraded = false;
    };

    void rollback(FileState& file, int& fd, const std::string& path);
    void drop_unsynced(FileState& file);

    std::map<std::string, FileState> files_;
    std::map<int, std::string> fd_paths_;
    std::map<std::string, std::string> redirects_;
    // Private path -> original path, so the inspection view never exposes the snapshot temp dir.
    std::map<std::string, std::string> display_paths_;
    std::set<std::string> captured_paths_;
    bool branch_active_{false};
    std::string branch_root_{};
    uint64_t next_new_file_{0};
    size_t tracked_bytes_{0};
    NodeId active_node_{0};
    uint64_t next_file_id_{1};
    bool degraded_{false};
    EventSink* event_sink_ = nullptr;
    const VirtualClock* clock_ = nullptr;
};

// The bytes a torn reboot zeroes at the end of the last synced range.
inline constexpr size_t kTornSectorBytes = 512;

} // namespace cosmos

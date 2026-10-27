#include "cosmos/snapshot.hpp"

#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cosmos {
namespace {

struct SnapshotCommand {
    uint64_t branch_index = 0;
    uint64_t forced_choice = 0;
    uint64_t schedule_seed = 0;
    uint8_t has_forced_choice = 0;
    uint8_t quit = 0;
    uint8_t has_schedule_seed = 0;
    uint8_t padding[5] = {};
};

struct SnapshotAck {
    uint8_t status = 0; // 0 = result file written, 1 = child died, 2 = checkpoint error
    int32_t signal = 0;
    uint64_t branch_index = 0;
};

static_assert(sizeof(SnapshotCommand) == 32);
static_assert(sizeof(SnapshotAck) == 16);

struct StoredFile {
    std::string source;
    std::string master;
};

struct StorageImage {
    std::vector<StoredFile> files;
    std::vector<OpenDescriptorInfo> descriptors;
};

struct BranchStorage {
    std::map<std::string, std::string> redirects;
    std::string directory;
};

std::atomic<uint64_t> g_next_snapshot_id{1};

// Metadata and storage copies are harness work. Clear the universe slot while they allocate so
// they neither consume fault draws nor become tracked blocks in the universe's heap.
struct CurrentSwap {
    Simulator* previous = nullptr;
    ~CurrentSwap() { Simulator::set_current(previous); }
};

bool write_exact(int fd, const void* data, size_t count) {
    const auto* bytes = static_cast<const char*>(data);
    size_t done = 0;
    while (done < count) {
        const ssize_t written = ::write(fd, bytes + done, count - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        done += static_cast<size_t>(written);
    }
    return true;
}

bool send_exact(int fd, const void* data, size_t count) {
    const auto* bytes = static_cast<const char*>(data);
    size_t done = 0;
    while (done < count) {
        const ssize_t sent = ::send(fd, bytes + done, count - done, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (sent == 0) return false;
        done += static_cast<size_t>(sent);
    }
    return true;
}

bool read_exact(int fd, void* data, size_t count) {
    auto* bytes = static_cast<char*>(data);
    size_t done = 0;
    while (done < count) {
        const ssize_t got = ::read(fd, bytes + done, count - done);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        done += static_cast<size_t>(got);
    }
    return true;
}

void set_cloexec(int fd) {
    if (fd >= 0) (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
}

std::string make_temp_dir() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string template_path =
        std::string(tmpdir != nullptr ? tmpdir : "/tmp") + "/cosmos-snapshot-XXXXXX";
    char* made = ::mkdtemp(template_path.data());
    return made == nullptr ? std::string() : std::string(made);
}

std::expected<StorageImage, std::string> capture_storage(Simulator& sim, const std::string& dir) {
    StorageImage image;
    image.descriptors = sim.storage().open_descriptors();
    for (const OpenDescriptorInfo& descriptor : image.descriptors) {
        constexpr int kUnsupported = O_PATH | O_DIRECT | O_ASYNC;
        if ((descriptor.flags & kUnsupported) != 0) {
            return std::unexpected("cannot branch a file descriptor with O_PATH/O_DIRECT/O_ASYNC");
        }
    }

    std::map<std::string, std::string> masters;
    auto add_file = [&](const std::string& source) -> std::expected<void, std::string> {
        if (masters.contains(source)) return {};
        std::error_code error;
        if (!std::filesystem::is_regular_file(source, error) || error) {
            return std::unexpected("storage path is not a regular file: " + source);
        }
        const std::string master = dir + "/master-" + std::to_string(masters.size()) + "-" +
                                   std::filesystem::path(source).filename().string();
        std::filesystem::copy_file(source, master,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return std::unexpected("could not capture storage file " + source + ": " +
                                   error.message());
        }
        masters.emplace(source, master);
        image.files.push_back(StoredFile{source, master});
        return {};
    };

    for (const std::string& source : sim.storage().tracked_paths()) {
        if (auto added = add_file(source); !added) return std::unexpected(added.error());
    }

    // In addition to the tracked model, capture regular descriptors the application opened before
    // the universe existed (or through an unwrapped path). Their paths are reachable through
    // procfs, so a branch can still get private copies instead of writing through the parent's
    // open file description.
    std::error_code scan_error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", scan_error)) {
        if (scan_error) break;
        const std::string name = entry.path().filename().string();
        char* end = nullptr;
        const long value = std::strtol(name.c_str(), &end, 10);
        if (end == name.c_str() || *end != '\0' || value < 3) continue;
        const int fd = static_cast<int>(value);
        if (sim.storage().is_tracked(fd)) continue;
        struct stat info{};
        if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        const std::string proc = "/proc/self/fd/" + std::to_string(fd);
        char target[4096];
        const ssize_t length = ::readlink(proc.c_str(), target, sizeof(target) - 1);
        if (length <= 0) {
            return std::unexpected("cannot isolate an untracked regular descriptor without a path");
        }
        target[length] = '\0';
        const std::string source(target);
        if (source.find(" (deleted)") != std::string::npos) {
            return std::unexpected("cannot isolate a deleted untracked regular descriptor");
        }
        if (auto added = add_file(source); !added) return std::unexpected(added.error());
        const off_t offset = ::lseek(fd, 0, SEEK_CUR);
        const int flags = ::fcntl(fd, F_GETFL);
        const int fd_flags = ::fcntl(fd, F_GETFD);
        if (offset < 0 || flags < 0 || fd_flags < 0) {
            return std::unexpected("could not inspect an untracked regular descriptor");
        }
        image.descriptors.push_back(OpenDescriptorInfo{fd, offset, flags, fd_flags, source});
    }
    return image;
}

// Build a private branch image. No branch ever opens a parent path or writes through a parent
// open-file description: each tracked descriptor is replaced with a descriptor on a branch copy,
// and later opens are redirected by StorageModel before the real open.
std::expected<BranchStorage, std::string>
prepare_branch_storage(const StorageImage& image, const std::string& dir, uint64_t branch_index) {
    BranchStorage branch;
    branch.directory = dir + "/branch-" + std::to_string(branch_index);
    std::error_code error;
    std::filesystem::create_directories(branch.directory, error);
    if (error) {
        return std::unexpected("could not create branch storage directory: " + error.message());
    }
    for (size_t index = 0; index < image.files.size(); ++index) {
        const StoredFile& file = image.files[index];
        const std::string copy = branch.directory + "/file-" + std::to_string(index) + "-" +
                                 std::filesystem::path(file.source).filename().string();
        std::filesystem::copy_file(file.master, copy,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return std::unexpected("could not create branch storage file " + file.source + ": " +
                                   error.message());
        }
        branch.redirects[file.source] = copy;
    }

    for (const OpenDescriptorInfo& descriptor : image.descriptors) {
        auto entry = branch.redirects.find(descriptor.path);
        if (entry == branch.redirects.end()) continue;
        constexpr int kPreservedFlags =
            O_ACCMODE | O_APPEND | O_SYNC | O_DSYNC | O_NONBLOCK | O_NOATIME | O_NOFOLLOW;
        const int flags = descriptor.flags & kPreservedFlags;
        const int private_fd = ::open(entry->second.c_str(), flags);
        if (private_fd < 0) {
            return std::unexpected("could not open branch storage fd: " +
                                   std::string(strerror(errno)));
        }
        if (::dup2(private_fd, descriptor.fd) < 0) {
            const std::string message =
                "could not replace branch storage fd: " + std::string(strerror(errno));
            ::close(private_fd);
            return std::unexpected(message);
        }
        ::close(private_fd);
        (void)::fcntl(descriptor.fd, F_SETFD, descriptor.fd_flags);
        if (::lseek(descriptor.fd, descriptor.offset, SEEK_SET) < 0) {
            return std::unexpected("could not restore branch storage offset: " +
                                   std::string(strerror(errno)));
        }
    }
    return branch;
}

void write_branch_error(const std::string& dir, uint64_t branch_index, const std::string& message) {
    const std::string path = dir + "/branch-" + std::to_string(branch_index) + ".error";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << message;
}

std::string read_branch_error(const std::string& dir, uint64_t branch_index) {
    const std::string path = dir + "/branch-" + std::to_string(branch_index) + ".error";
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    (void)::unlink(path.c_str());
    return text.str();
}

struct NestedRecorder {
    SnapshotInfo parent{};
    uint64_t parent_branch_index = kNoBranch;
    uint64_t next = 0;
    std::vector<SnapshotInfo> snapshots{};
    std::vector<NestedBranch> branches{};
};

thread_local NestedRecorder* t_nested = nullptr;

void append_info(std::string& bytes, const SnapshotInfo& info) {
    detail::put_field(bytes, info.id);
    detail::put_field(bytes, info.parent_id);
    detail::put_field(bytes, info.parent_branch_index);
    detail::put_field(bytes, info.seed);
    detail::put_field(bytes, info.at.ns);
    detail::put_field(bytes, info.tasks);
    detail::put_field(bytes, info.ready);
    detail::put_field(bytes, info.blocked);
    detail::put_field(bytes, info.ready_choices);
    detail::put_field(bytes, info.heap_allocations);
    detail::put_field(bytes, info.heap_bytes);
    detail::put_field(bytes, info.endpoints);
    detail::put_field(bytes, info.pending_packets);
    detail::put_field(bytes, info.storage_files);
    detail::put_field(bytes, info.trace_decisions);
    detail::put_field(bytes, info.trace_entries);
    detail::put_field(bytes, static_cast<uint8_t>(info.coverage_present ? 1 : 0));
    detail::put_field(bytes, info.coverage_edges);
    detail::put_field(bytes, info.coverage_hash);
    detail::put_field(bytes, static_cast<uint8_t>(info.faults_quiet ? 1 : 0));
    detail::put_text(bytes, info.method);
}

bool take_field(std::string_view bytes, size_t& offset, void* out, size_t size) {
    if (size > bytes.size() - offset) return false;
    std::memcpy(out, bytes.data() + offset, size);
    offset += size;
    return true;
}

bool take_text(std::string_view bytes, size_t& offset, std::string& text) {
    uint64_t length = 0;
    if (!take_field(bytes, offset, &length, sizeof(length))) return false;
    if (length > bytes.size() - offset) return false;
    text.assign(bytes.data() + offset, static_cast<size_t>(length));
    offset += static_cast<size_t>(length);
    return true;
}

SnapshotInfo take_info(std::string_view bytes, size_t& offset) {
    SnapshotInfo info;
    uint8_t coverage_present = 0;
    uint8_t faults_quiet = 0;
    take_field(bytes, offset, &info.id, sizeof(info.id));
    take_field(bytes, offset, &info.parent_id, sizeof(info.parent_id));
    take_field(bytes, offset, &info.parent_branch_index, sizeof(info.parent_branch_index));
    take_field(bytes, offset, &info.seed, sizeof(info.seed));
    take_field(bytes, offset, &info.at.ns, sizeof(info.at.ns));
    take_field(bytes, offset, &info.tasks, sizeof(info.tasks));
    take_field(bytes, offset, &info.ready, sizeof(info.ready));
    take_field(bytes, offset, &info.blocked, sizeof(info.blocked));
    take_field(bytes, offset, &info.ready_choices, sizeof(info.ready_choices));
    take_field(bytes, offset, &info.heap_allocations, sizeof(info.heap_allocations));
    take_field(bytes, offset, &info.heap_bytes, sizeof(info.heap_bytes));
    take_field(bytes, offset, &info.endpoints, sizeof(info.endpoints));
    take_field(bytes, offset, &info.pending_packets, sizeof(info.pending_packets));
    take_field(bytes, offset, &info.storage_files, sizeof(info.storage_files));
    take_field(bytes, offset, &info.trace_decisions, sizeof(info.trace_decisions));
    take_field(bytes, offset, &info.trace_entries, sizeof(info.trace_entries));
    take_field(bytes, offset, &coverage_present, sizeof(coverage_present));
    take_field(bytes, offset, &info.coverage_edges, sizeof(info.coverage_edges));
    take_field(bytes, offset, &info.coverage_hash, sizeof(info.coverage_hash));
    take_field(bytes, offset, &faults_quiet, sizeof(faults_quiet));
    take_text(bytes, offset, info.method);
    info.coverage_present = coverage_present != 0;
    info.faults_quiet = faults_quiet != 0;
    return info;
}

void append_branch(std::string& bytes, const NestedBranch& branch) {
    append_info(bytes, branch.snapshot);
    detail::put_field(bytes, branch.context.branch_index);
    detail::put_field(bytes, branch.context.snapshot_id);
    detail::put_field(bytes, branch.context.parent_snapshot_id);
    detail::put_field(bytes, static_cast<uint8_t>(branch.context.has_forced_choice ? 1 : 0));
    detail::put_field(bytes, branch.context.forced_choice);
    detail::put_field(bytes, static_cast<uint8_t>(branch.context.has_schedule_seed ? 1 : 0));
    detail::put_field(bytes, branch.context.schedule_seed);
    detail::put_field(bytes, static_cast<uint8_t>(branch.died ? 1 : 0));
    detail::put_field(bytes, branch.signal);
    detail::put_field(bytes, static_cast<uint8_t>(branch.forced_choice_applied ? 1 : 0));
    detail::put_text(bytes, branch.threw);
    detail::put_text(bytes, branch.error);
    detail::put_field(bytes, branch.report.seed);
    detail::put_field(bytes, static_cast<uint8_t>(branch.report.no_check_failed ? 1 : 0));
    detail::put_field(bytes, static_cast<uint64_t>(branch.report.checks.size()));
    for (const CheckResult& check : branch.report.checks) {
        detail::put_text(bytes, check.id);
        detail::put_text(bytes, check.detail);
        detail::put_field(bytes, static_cast<uint8_t>(check.passed ? 1 : 0));
        detail::put_field(bytes, check.at.ns);
    }
    detail::put_field(bytes, static_cast<uint64_t>(branch.report.active_allocations));
    detail::put_text(bytes, branch.report.ledger_dump);
    detail::put_field(bytes, branch.ended_at.ns);
    detail::put_field(bytes, branch.wall_ns);
    detail::put_field(bytes, static_cast<uint8_t>(branch.traced ? 1 : 0));
    detail::put_field(bytes, branch.trace_hash);
    detail::put_field(bytes, branch.trace_capacity);
    detail::put_field(bytes, branch.trace_dropped);
    detail::put_text(bytes, branch.trace_bytes);
    detail::put_field(bytes, static_cast<uint8_t>(branch.coverage_present ? 1 : 0));
    detail::put_text(bytes, branch.coverage_method);
    detail::put_field(bytes, branch.coverage_edges);
    detail::put_field(bytes, branch.coverage_hash);
}

void record_nested_result(NestedRecorder& recorder, const BranchResult& result);

struct NestedResultGuard {
    BranchResult& result;
    ~NestedResultGuard() {
        if (t_nested != nullptr) record_nested_result(*t_nested, result);
    }
};

std::string encode_nested(const NestedRecorder& recorder) {
    std::string bytes;
    detail::put_field(bytes, static_cast<uint64_t>(recorder.snapshots.size()));
    for (const SnapshotInfo& info : recorder.snapshots)
        append_info(bytes, info);
    detail::put_field(bytes, static_cast<uint64_t>(recorder.branches.size()));
    for (const NestedBranch& branch : recorder.branches)
        append_branch(bytes, branch);
    return bytes;
}

std::optional<std::pair<std::vector<SnapshotInfo>, std::vector<NestedBranch>>>
decode_nested(std::string_view bytes) {
    if (bytes.empty())
        return std::make_pair(std::vector<SnapshotInfo>{}, std::vector<NestedBranch>{});
    size_t offset = 0;
    uint64_t snapshot_count = 0;
    if (!take_field(bytes, offset, &snapshot_count, sizeof(snapshot_count))) return std::nullopt;
    if (snapshot_count > 4096) return std::nullopt;
    std::vector<SnapshotInfo> snapshots;
    snapshots.reserve(static_cast<size_t>(snapshot_count));
    for (uint64_t index = 0; index < snapshot_count; ++index) {
        snapshots.push_back(take_info(bytes, offset));
    }
    uint64_t branch_count = 0;
    if (!take_field(bytes, offset, &branch_count, sizeof(branch_count))) return std::nullopt;
    if (branch_count > 4096) return std::nullopt;
    std::vector<NestedBranch> branches;
    branches.reserve(static_cast<size_t>(branch_count));
    for (uint64_t index = 0; index < branch_count; ++index) {
        NestedBranch branch;
        branch.snapshot = take_info(bytes, offset);
        uint8_t forced = 0;
        uint8_t schedule = 0;
        uint8_t died = 0;
        uint8_t applied = 0;
        uint8_t no_fail = 0;
        uint8_t traced = 0;
        uint8_t coverage = 0;
        take_field(bytes, offset, &branch.context.branch_index,
                   sizeof(branch.context.branch_index));
        take_field(bytes, offset, &branch.context.snapshot_id, sizeof(branch.context.snapshot_id));
        take_field(bytes, offset, &branch.context.parent_snapshot_id,
                   sizeof(branch.context.parent_snapshot_id));
        take_field(bytes, offset, &forced, sizeof(forced));
        take_field(bytes, offset, &branch.context.forced_choice,
                   sizeof(branch.context.forced_choice));
        take_field(bytes, offset, &schedule, sizeof(schedule));
        take_field(bytes, offset, &branch.context.schedule_seed,
                   sizeof(branch.context.schedule_seed));
        take_field(bytes, offset, &died, sizeof(died));
        take_field(bytes, offset, &branch.signal, sizeof(branch.signal));
        take_field(bytes, offset, &applied, sizeof(applied));
        take_text(bytes, offset, branch.threw);
        take_text(bytes, offset, branch.error);
        take_field(bytes, offset, &branch.report.seed, sizeof(branch.report.seed));
        take_field(bytes, offset, &no_fail, sizeof(no_fail));
        uint64_t check_count = 0;
        take_field(bytes, offset, &check_count, sizeof(check_count));
        if (check_count > 1'000'000) return std::nullopt;
        for (uint64_t c = 0; c < check_count; ++c) {
            CheckResult check;
            uint8_t passed = 0;
            take_text(bytes, offset, check.id);
            take_text(bytes, offset, check.detail);
            take_field(bytes, offset, &passed, sizeof(passed));
            take_field(bytes, offset, &check.at.ns, sizeof(check.at.ns));
            check.passed = passed != 0;
            branch.report.checks.push_back(std::move(check));
        }
        uint64_t allocations = 0;
        take_field(bytes, offset, &allocations, sizeof(allocations));
        branch.report.active_allocations = static_cast<size_t>(allocations);
        take_text(bytes, offset, branch.report.ledger_dump);
        take_field(bytes, offset, &branch.ended_at.ns, sizeof(branch.ended_at.ns));
        take_field(bytes, offset, &branch.wall_ns, sizeof(branch.wall_ns));
        take_field(bytes, offset, &traced, sizeof(traced));
        take_field(bytes, offset, &branch.trace_hash, sizeof(branch.trace_hash));
        take_field(bytes, offset, &branch.trace_capacity, sizeof(branch.trace_capacity));
        take_field(bytes, offset, &branch.trace_dropped, sizeof(branch.trace_dropped));
        take_text(bytes, offset, branch.trace_bytes);
        take_field(bytes, offset, &coverage, sizeof(coverage));
        take_text(bytes, offset, branch.coverage_method);
        take_field(bytes, offset, &branch.coverage_edges, sizeof(branch.coverage_edges));
        take_field(bytes, offset, &branch.coverage_hash, sizeof(branch.coverage_hash));
        branch.context.has_forced_choice = forced != 0;
        branch.context.has_schedule_seed = schedule != 0;
        branch.died = died != 0;
        branch.forced_choice_applied = applied != 0;
        branch.report.no_check_failed = no_fail != 0;
        branch.traced = traced != 0;
        branch.coverage_present = coverage != 0;
        branches.push_back(std::move(branch));
    }
    return std::make_pair(std::move(snapshots), std::move(branches));
}

NestedBranch nested_from_result(const BranchResult& result) {
    NestedBranch branch;
    branch.snapshot = result.snapshot;
    branch.context = result.context;
    branch.died = result.died;
    branch.signal = result.signal;
    branch.forced_choice_applied = result.forced_choice_applied;
    branch.threw = result.threw;
    branch.error = result.error;
    branch.report = result.report;
    branch.ended_at = result.ended_at;
    branch.wall_ns = result.wall_ns;
    branch.traced = result.traced;
    branch.trace_hash = result.trace_hash;
    branch.trace_capacity = result.trace_capacity;
    branch.trace_dropped = result.trace_dropped;
    branch.trace_bytes = result.trace_bytes;
    branch.coverage_present = result.coverage_present;
    branch.coverage_method = result.coverage_method;
    branch.coverage_edges = result.coverage.edges;
    branch.coverage_hash = result.coverage.hash;
    return branch;
}

void record_nested_result(NestedRecorder& recorder, const BranchResult& result) {
    recorder.branches.push_back(nested_from_result(result));
    for (const SnapshotInfo& info : result.nested_snapshots)
        recorder.snapshots.push_back(info);
    for (const NestedBranch& branch : result.nested_branches)
        recorder.branches.push_back(branch);
}

SnapshotInfo make_info(Scenario& scenario, uint64_t id, uint64_t parent_id,
                       uint64_t parent_branch_index) {
    Simulator& sim = scenario.simulator();
    SnapshotInfo info;
    info.id = id;
    info.parent_id = parent_id;
    info.parent_branch_index = parent_branch_index;
    info.seed = sim.seed();
    info.at = sim.now();
    const std::vector<TaskInfo> tasks = sim.scheduler().task_infos();
    info.tasks = tasks.size();
    for (const TaskInfo& task : tasks) {
        switch (task.state) {
        case Task::State::Ready:
            ++info.ready;
            break;
        case Task::State::Running:
            break;
        case Task::State::Done:
            break;
        default:
            ++info.blocked;
            break;
        }
    }
    info.ready_choices = sim.scheduler().ready_count();
    info.heap_allocations = sim.heap().active_count();
    info.heap_bytes = sim.heap().active_bytes();
    const std::vector<EndpointInfo> endpoints = sim.net().endpoint_infos();
    info.endpoints = endpoints.size();
    for (const EndpointInfo& endpoint : endpoints) {
        info.pending_packets += endpoint.rx_packets + endpoint.pending_accepts;
    }
    info.storage_files = sim.storage().file_count();
    if (const auto* injector = sim.injector_or_null()) {
        info.trace_decisions = injector->decisions();
        info.trace_entries = injector->trace().size();
        info.faults_quiet = injector->faults_quiet();
    }
    // Snapshot coverage is not a stable observation: the bitmap may belong to a prior universe or
    // to harness code. Branch results carry the coverage that belongs to the branch.
    info.coverage_present = false;
    return info;
}

BranchResult result_from_outcome(const SnapshotInfo& snapshot, const BranchContext& context,
                                 detail::UniverseOutcome&& outcome) {
    BranchResult result;
    result.context = context;
    result.snapshot = snapshot;
    result.report = std::move(outcome.report);
    result.died = outcome.died;
    result.signal = outcome.signal;
    result.forced_choice_applied = outcome.forced_choice_used;
    result.threw = std::move(outcome.threw);
    result.ended_at = outcome.ended_at;
    result.wall_ns = outcome.wall_ns;
    result.traced = outcome.traced;
    result.trace_hash = outcome.trace_hash;
    result.trace_capacity = outcome.trace_capacity;
    result.trace_dropped = outcome.trace_dropped;
    result.trace_bytes = std::move(outcome.trace_bytes);
    result.coverage_present = outcome.coverage_present;
    result.coverage_method = std::string(coverage_method());
    result.coverage = std::move(outcome.coverage);
    if (auto nested = decode_nested(outcome.nested_payload); nested.has_value()) {
        result.nested_snapshots = std::move(nested->first);
        result.nested_branches = std::move(nested->second);
    }
    return result;
}

void run_branch_child(Simulator& sim, Scenario& scenario, const BranchBody& body,
                      const BranchContext& context, const SnapshotInfo& snapshot_info,
                      const std::string& branch_root,
                      const std::map<std::string, std::string>& redirects, uint64_t trace_start,
                      int result_fd, int cmd_read, int ack_write) {
    ::close(ack_write);
    ::close(cmd_read);
    // Redirect the branch's storage before any application allocation can be tracked or faulted.
    Simulator::set_current(nullptr);
    sim.storage().begin_branch_storage(branch_root, redirects);
    Simulator::set_current(&sim);
    NestedRecorder recorder;
    recorder.parent = snapshot_info;
    recorder.parent_branch_index = context.branch_index;
    t_nested = &recorder;
    scenario.begin_branch_report();
    coverage_reset();
    sim.scheduler().clear_forced_choice();
    if (context.has_forced_choice) sim.scheduler().force_next_choice(context.forced_choice);
    if (context.has_schedule_seed) sim.scheduler().reseed_schedule(context.schedule_seed);

    std::string threw;
    const auto wall_start = std::chrono::steady_clock::now();
    try {
        if (body) body(scenario, context);
    } catch (const std::exception& error) {
        threw = error.what();
    } catch (...) {
        threw = "unknown exception";
    }
    // Scenario bookkeeping allocates outside its CurrentGuard; clear the slot so a branch check
    // never becomes a tracked application block or consumes a live fault draw.
    Simulator::set_current(nullptr);
    try {
        scenario.quiesce();
    } catch (const std::exception& error) {
        if (threw.empty()) threw = std::string("quiesce: ") + error.what();
    } catch (...) {
        if (threw.empty()) threw = "quiesce: unknown exception";
    }

    detail::UniverseOutcome outcome;
    outcome.index = context.branch_index;
    outcome.seed = sim.seed();
    outcome.threw = threw;
    outcome.report = scenario.report();
    outcome.ended_at = sim.now();
    outcome.wall_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - wall_start)
                                                .count());
    if (const auto* injector = sim.injector_or_null()) {
        outcome.has_config = true;
        outcome.config_json = config_json(injector->config());
        outcome.config_hash = config_fingerprint(injector->config());
        for (const LedgerEntry& entry : injector->ledger()) {
            outcome.ledger_entries.push_back(entry);
        }
        outcome.ledger_dropped = injector->ledger().dropped();
        const FaultTrace& trace = injector->trace();
        outcome.traced = trace.enabled();
        if (outcome.traced) {
            // The inherited trace includes pre-snapshot decisions. A branch identity covers only
            // its continuation; the original entries still belong to the parent run.
            FaultTrace suffix(trace.capacity());
            suffix.set_universe_seed(trace.universe_seed());
            for (size_t index = static_cast<size_t>(std::min<uint64_t>(trace_start, trace.size()));
                 index < trace.size(); ++index) {
                suffix.record(trace[index]);
            }
            outcome.trace_capacity = suffix.capacity();
            outcome.trace_dropped = suffix.dropped();
            outcome.trace_bytes = encode_trace(suffix);
            outcome.trace_hash = detail::fnv1a_64(outcome.trace_bytes);
        }
    }
    outcome.coverage_present = coverage_method() != "none";
    outcome.forced_choice_used = sim.scheduler().forced_choice_used();
    if (outcome.coverage_present) outcome.coverage = coverage_snapshot();
    outcome.nested_payload = encode_nested(recorder);
    t_nested = nullptr;

    Simulator::set_current(nullptr);
    const std::string record = detail::serialize_outcome(outcome);
    std::string bytes;
    detail::put_field(bytes, static_cast<uint64_t>(record.size()));
    bytes += record;
    if (!write_exact(result_fd, bytes.data(), bytes.size())) ::_exit(1);
    ::close(result_fd);
    ::_exit(0);
}

[[noreturn]] void checkpoint_loop(Simulator& sim, Scenario& scenario, BranchBody body,
                                  SnapshotInfo info, StorageImage storage, const std::string& dir,
                                  int cmd_read, int ack_write) {
    if (!body) {
        body = [](Scenario& branch_scenario, const BranchContext&) { branch_scenario.drain(); };
    }
    Simulator::set_current(nullptr);
    for (;;) {
        SnapshotCommand command;
        if (!read_exact(cmd_read, &command, sizeof(command)) || command.quit != 0) break;

        auto branch_storage = prepare_branch_storage(storage, dir, command.branch_index);
        if (!branch_storage) {
            write_branch_error(dir, command.branch_index, branch_storage.error());
            std::error_code error;
            std::filesystem::remove_all(dir + "/branch-" + std::to_string(command.branch_index),
                                        error);
            SnapshotAck ack{.status = 2, .signal = 0, .branch_index = command.branch_index};
            if (!send_exact(ack_write, &ack, sizeof(ack))) break;
            continue;
        }

        const std::string result_path =
            dir + "/branch-" + std::to_string(command.branch_index) + ".bin";
        const int result_fd = ::open(result_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (result_fd < 0) {
            write_branch_error(dir, command.branch_index,
                               "could not create the branch result file");
            std::error_code error;
            std::filesystem::remove_all(branch_storage->directory, error);
            SnapshotAck ack{.status = 2, .signal = 0, .branch_index = command.branch_index};
            if (!send_exact(ack_write, &ack, sizeof(ack))) break;
            continue;
        }

        BranchContext context;
        context.branch_index = command.branch_index;
        context.snapshot_id = info.id;
        context.parent_snapshot_id = info.parent_id;
        context.has_forced_choice = command.has_forced_choice != 0;
        context.forced_choice = command.forced_choice;
        context.has_schedule_seed = command.has_schedule_seed != 0;
        context.schedule_seed = command.schedule_seed;

        const pid_t pid = ::fork();
        if (pid == 0) {
            run_branch_child(sim, scenario, body, context, info, branch_storage->directory,
                             branch_storage->redirects, info.trace_entries, result_fd, cmd_read,
                             ack_write);
        }
        ::close(result_fd);
        if (pid < 0) {
            write_branch_error(dir, command.branch_index, "fork() failed for the branch");
            std::error_code error;
            std::filesystem::remove_all(branch_storage->directory, error);
            SnapshotAck ack{.status = 2, .signal = 0, .branch_index = command.branch_index};
            if (!send_exact(ack_write, &ack, sizeof(ack))) break;
            continue;
        }

        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        std::error_code error;
        std::filesystem::remove_all(branch_storage->directory, error);
        struct stat result_stat{};
        const bool result_present =
            ::stat(result_path.c_str(), &result_stat) == 0 && result_stat.st_size > 0;
        SnapshotAck ack{};
        ack.branch_index = command.branch_index;
        if (waited < 0) {
            write_branch_error(dir, command.branch_index, "waitpid() failed for the branch");
            ack.status = 2;
        } else if (WIFSIGNALED(status)) {
            ack.status = 1;
            ack.signal = WTERMSIG(status);
        } else if (WEXITSTATUS(status) != 0 || !result_present) {
            ack.status = 3;
        } else {
            ack.status = 0;
        }
        if (!send_exact(ack_write, &ack, sizeof(ack))) break;
    }
    ::close(cmd_read);
    ::close(ack_write);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    ::_exit(0);
}

} // namespace

struct Snapshot::Impl {
    pid_t pid = -1;
    int cmd_fd = -1;
    int ack_fd = -1;
    std::string dir;
    SnapshotInfo info{};
    std::set<uint64_t> used_branches{};
    bool active = false;
};

Snapshot::Snapshot(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Snapshot::Snapshot(Snapshot&& other) noexcept = default;

Snapshot& Snapshot::operator=(Snapshot&& other) noexcept {
    if (this == &other) return *this;
    if (impl_) {
        auto* previous = Simulator::current();
        CurrentSwap swap{previous};
        Simulator::set_current(nullptr);
        const pid_t pid = impl_->pid;
        if (pid > 0) {
            SnapshotCommand quit;
            quit.quit = 1;
            (void)send_exact(impl_->cmd_fd, &quit, sizeof(quit));
            (void)::close(impl_->cmd_fd);
            (void)::close(impl_->ack_fd);
            (void)::waitpid(pid, nullptr, 0);
            std::error_code error;
            std::filesystem::remove_all(impl_->dir, error);
        }
    }
    impl_ = std::move(other.impl_);
    return *this;
}

Snapshot::~Snapshot() {
    if (!impl_ || impl_->pid <= 0) return;
    auto* previous = Simulator::current();
    CurrentSwap swap{previous};
    Simulator::set_current(nullptr);
    SnapshotCommand quit;
    quit.quit = 1;
    (void)send_exact(impl_->cmd_fd, &quit, sizeof(quit));
    (void)::close(impl_->cmd_fd);
    (void)::close(impl_->ack_fd);
    (void)::waitpid(impl_->pid, nullptr, 0);
    std::error_code error;
    std::filesystem::remove_all(impl_->dir, error);
}

const SnapshotInfo& Snapshot::info() const {
    static const SnapshotInfo empty{};
    return impl_ ? impl_->info : empty;
}

std::expected<Snapshot, std::string> Snapshot::capture(Scenario& scenario, BranchBody body,
                                                       uint64_t parent_id) {
    Simulator& sim = scenario.simulator();
    if (!sim.scheduler().can_capture()) {
        return std::unexpected(
            "snapshot capture requires the main fiber and no active drain; capture between "
            "workload phases instead of inside a fiber or a running drain");
    }
    // Keep the harness window clear through temp-dir creation, metadata, storage copies and Impl
    // construction. The branch child re-establishes the universe slot in run_branch_child().
    auto* previous = Simulator::current();
    CurrentSwap swap{previous};
    Simulator::set_current(nullptr);
    const std::string dir = make_temp_dir();
    if (dir.empty()) return std::unexpected("could not create a snapshot directory");

    uint64_t id = g_next_snapshot_id.fetch_add(1);
    uint64_t info_parent_id = parent_id;
    uint64_t parent_branch_index = kNoBranch;
    if (t_nested != nullptr) {
        NestedRecorder& recorder = *t_nested;
        id = derive_seed(recorder.parent.id ^ (recorder.parent_branch_index + 1), ++recorder.next);
        // Keep nested ids out of the small top-level counter space even if the mixer lands low.
        id |= (uint64_t{1} << 63U);
        if (id == 0) id = uint64_t{1} << 63U;
        info_parent_id = recorder.parent.id;
        parent_branch_index = recorder.parent_branch_index;
    }
    SnapshotInfo info = make_info(scenario, id, info_parent_id, parent_branch_index);
    auto captured = capture_storage(sim, dir);
    if (!captured) {
        std::error_code error;
        std::filesystem::remove_all(dir, error);
        return std::unexpected(captured.error());
    }
    StorageImage storage = std::move(*captured);

    int cmd_pair[2] = {-1, -1};
    int ack_pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, cmd_pair) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, ack_pair) != 0) {
        if (cmd_pair[0] >= 0) ::close(cmd_pair[0]);
        if (cmd_pair[1] >= 0) ::close(cmd_pair[1]);
        if (ack_pair[0] >= 0) ::close(ack_pair[0]);
        if (ack_pair[1] >= 0) ::close(ack_pair[1]);
        std::error_code error;
        std::filesystem::remove_all(dir, error);
        return std::unexpected("could not create snapshot sockets");
    }
    for (int fd : {cmd_pair[0], cmd_pair[1], ack_pair[0], ack_pair[1]})
        set_cloexec(fd);

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(cmd_pair[1]);
        ::close(ack_pair[0]);
        checkpoint_loop(sim, scenario, std::move(body), info, std::move(storage), dir, cmd_pair[0],
                        ack_pair[1]);
    }
    ::close(cmd_pair[0]);
    ::close(ack_pair[1]);
    if (pid < 0) {
        ::close(cmd_pair[1]);
        ::close(ack_pair[0]);
        std::error_code error;
        std::filesystem::remove_all(dir, error);
        return std::unexpected("fork() failed while creating a snapshot");
    }

    auto impl = std::make_unique<Impl>();
    impl->pid = pid;
    impl->cmd_fd = cmd_pair[1];
    impl->ack_fd = ack_pair[0];
    impl->dir = dir;
    impl->info = info;
    impl->active = true;
    if (t_nested != nullptr) t_nested->snapshots.push_back(info);
    return Snapshot(std::move(impl));
}

BranchResult Snapshot::explore(const BranchContext& requested) {
    BranchResult result;
    if (!impl_ || !impl_->active) {
        result.error = "snapshot is not active";
        return result;
    }
    auto* previous = Simulator::current();
    CurrentSwap swap{previous};
    Simulator::set_current(nullptr);
    BranchContext context = requested;
    context.snapshot_id = impl_->info.id;
    if (context.parent_snapshot_id == 0) context.parent_snapshot_id = impl_->info.parent_id;
    result.snapshot = impl_->info;
    result.report.seed = impl_->info.seed;
    NestedResultGuard nested_guard{result};
    if (context.has_forced_choice && context.forced_choice >= impl_->info.ready_choices) {
        result.context = context;
        result.error = "forced choice is outside the captured ready set";
        return result;
    }
    if (!impl_->used_branches.insert(context.branch_index).second) {
        result.context = context;
        result.error = "branch index was already explored for this snapshot";
        return result;
    }

    SnapshotCommand command;
    command.branch_index = context.branch_index;
    command.forced_choice = context.forced_choice;
    command.schedule_seed = context.schedule_seed;
    command.has_forced_choice = context.has_forced_choice ? 1 : 0;
    command.has_schedule_seed = context.has_schedule_seed ? 1 : 0;
    if (!send_exact(impl_->cmd_fd, &command, sizeof(command))) {
        impl_->active = false;
        result.error = "could not send a branch command to the checkpoint";
        return result;
    }

    SnapshotAck ack;
    if (!read_exact(impl_->ack_fd, &ack, sizeof(ack))) {
        impl_->active = false;
        result.error = "could not read the branch acknowledgement";
        return result;
    }
    result.context = context;
    if (ack.status == 1 || ack.status == 3) {
        result.died = true;
        result.signal = ack.status == 1 ? ack.signal : 0;
        if (ack.status == 3) result.error = "branch exited without a result";
        return result;
    }
    if (ack.status == 2) {
        result.error = read_branch_error(impl_->dir, context.branch_index);
        if (result.error.empty()) result.error = "the checkpoint could not start the branch";
        return result;
    }

    const std::string result_path =
        impl_->dir + "/branch-" + std::to_string(context.branch_index) + ".bin";
    const int fd = ::open(result_path.c_str(), O_RDONLY);
    if (fd < 0) {
        result.error = "the branch result file is missing";
        return result;
    }
    detail::UniverseOutcome outcome;
    detail::OutcomeReader reader(fd);
    const bool read = reader.next(outcome);
    ::close(fd);
    (void)::unlink(result_path.c_str());
    if (!read) {
        result.error = "the branch result file could not be decoded";
        return result;
    }
    result = result_from_outcome(impl_->info, context, std::move(outcome));
    return result;
}

} // namespace cosmos

#include "cosmos/storage.hpp"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

// The bytes stay in the host file; this model only remembers how to put the old ones back after
// a reboot. Its syscalls are openat, pwrite, fstat, ftruncate, close and fcntl. close and fcntl
// are link-wrapped, but neither re-enters the injector: __wrap_close only forgets the fd (it
// draws no fault, so a model close is benign) and __wrap_fcntl passes a host fd straight to the
// real call. Degradation stays sticky: once a file or the universe is degraded, discard for it is
// a no-op for the rest of the run.

namespace cosmos {

namespace {

void record_storage(EventSink* sink, const VirtualClock* clock, EventKind kind, NodeId node,
                    EventPayload payload, uint64_t correlation = kNoCorrelation) {
    if (sink == nullptr || !sink->enabled() || !sink->seeded()) return;
    Event event;
    event.at = clock != nullptr ? clock->now() : Time::zero();
    event.kind = kind;
    event.node = node;
    event.task = kNoTask;
    event.correlation_id = correlation;
    event.payload = payload;
    sink->record(event);
}

// Reports the kernel size; false when fstat fails, because "unknown" must not read as "empty".
bool size_of(int fd, off_t& size) {
    struct stat info{};
    if (fstat(fd, &info) != 0) return false;
    size = info.st_size;
    return true;
}

// Only a descriptor that may write can put the old bytes back.
bool writable(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return false;
    const int mode = flags & O_ACCMODE;
    return mode == O_WRONLY || mode == O_RDWR;
}

// Raw syscalls: a test binary may link --wrap=pwrite/ftruncate, which also rewrites libcosmos's
// own references. A rollback that re-entered the injector would draw a fault and record writes
// into the very vector it is draining.
ssize_t host_pwrite(int fd, const void* buffer, size_t size, off_t offset) {
    return static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, buffer, size, offset));
}

int host_ftruncate(int fd, off_t size) {
    return static_cast<int>(::syscall(SYS_ftruncate, fd, size));
}

} // namespace

void StorageModel::track_open(int fd, std::string path, bool truncate) {
    off_t actual = 0;
    if (!size_of(fd, actual)) {
        // Treating a failed fstat as zero would let a reboot empty a non-empty file.
        fd_paths_.erase(fd);
        auto known = files_.find(path);
        if (known != files_.end()) known->second.degraded = true;
        degraded_ = true;
        return;
    }

    // A reused fd stops pointing at whatever path it held before.
    fd_paths_[fd] = path;

    auto [entry, inserted] = files_.try_emplace(std::move(path));
    FileState& file = entry->second;
    if (inserted) {
        // The owner is the opener; every later write re-homes the file to its writer.
        file.id = next_file_id_++;
        file.node = active_node_;
        file.durable_size = actual;
    } else if (actual < file.durable_size) {
        // The file shrank behind the model's back (unlink+recreate, external truncate).
        drop_unsynced(file);
        file.last_sync.reset();
        file.durable_size = actual;
    }
    // Never raise durable_size here: another fd's un-synced append is not durable.

    if (truncate) {
        // O_TRUNC threw the previous contents away, so the durable state is empty too.
        drop_unsynced(file);
        file.durable_size = 0;
        file.last_sync.reset();
    }
}

void StorageModel::forget_fd(int fd) { fd_paths_.erase(fd); }

void StorageModel::record_write(int fd, off_t offset, std::string_view previous, size_t length,
                                NodeId node) {
    auto fd_entry = fd_paths_.find(fd);
    if (fd_entry == fd_paths_.end()) return;
    auto file_entry = files_.find(fd_entry->second);
    if (file_entry == files_.end()) return;

    FileState& file = file_entry->second;
    if (file.degraded) return; // discard is already a no-op for this file
    file.node = node;          // the writer owns the file from now on

    file.unsynced.push_back(WriteRecord{offset, length, std::string(previous)});
    tracked_bytes_ += previous.size();
    record_storage(event_sink_, clock_, EventKind::storage_dirty, file.node,
                   EventPayload{.subject = file.id,
                                .related = static_cast<uint64_t>(fd),
                                .value = static_cast<uint64_t>(length),
                                .extra = static_cast<uint64_t>(tracked_bytes_),
                                .amount_ns = static_cast<int64_t>(offset)});

    if (tracked_bytes_ > kMaxTrackedBytes) {
        // Past the budget the file stops being discard-tracked: holding half a history would
        // restore a state the crash never produced.
        degraded_ = true;
        file.degraded = true;
        drop_unsynced(file);
        record_storage(
            event_sink_, clock_, EventKind::storage_degraded, file.node,
            EventPayload{.subject = file.id, .value = static_cast<uint64_t>(tracked_bytes_)});
    }
}

void StorageModel::commit_fsync(int fd) {
    auto fd_entry = fd_paths_.find(fd);
    if (fd_entry == fd_paths_.end()) return;
    auto file_entry = files_.find(fd_entry->second);
    if (file_entry == files_.end()) return;

    FileState& file = file_entry->second;
    off_t actual = 0;
    if (!size_of(fd, actual)) {
        // Without the real size the fsync boundary is unknown; stop simulating discard.
        degraded_ = true;
        file.degraded = true;
        drop_unsynced(file);
        file.last_sync.reset();
        record_storage(event_sink_, clock_, EventKind::storage_degraded, file.node,
                       EventPayload{.subject = file.id});
        return;
    }
    // fsync's boundary is the file's size as the kernel reports it.
    const uint64_t committed = static_cast<uint64_t>(file.unsynced.size());
    file.durable_size = actual;
    if (!file.unsynced.empty()) file.last_sync = file.unsynced.back();
    drop_unsynced(file);
    record_storage(event_sink_, clock_, EventKind::storage_durable, file.node,
                   EventPayload{.subject = file.id,
                                .related = static_cast<uint64_t>(fd),
                                .value = static_cast<uint64_t>(actual),
                                .extra = committed});
}

void StorageModel::record_truncate(int fd, off_t new_size) {
    auto fd_entry = fd_paths_.find(fd);
    if (fd_entry == fd_paths_.end()) return;
    auto file_entry = files_.find(fd_entry->second);
    if (file_entry == files_.end()) return;

    FileState& file = file_entry->second;
    // A record starting at or past the cut can never be rolled back into existence.
    std::erase_if(file.unsynced, [this, new_size](const WriteRecord& record) {
        if (record.offset < new_size) return false;
        tracked_bytes_ -= record.previous.size();
        return true;
    });

    // Growth only adds un-synced zeros; it never makes an earlier write durable.
    if (new_size >= file.durable_size) return;

    if (file.last_sync.has_value()) {
        WriteRecord& last = *file.last_sync;
        if (last.offset >= new_size) {
            file.last_sync.reset();
        } else if (last.offset + static_cast<off_t>(last.length) > new_size) {
            // A torn reboot may only zero what is still inside the file.
            last.length = static_cast<size_t>(new_size - last.offset);
        }
    }
    file.durable_size = new_size;
}

void StorageModel::reboot(NodeId node, bool torn) {
    for (auto& [path, file] : files_) {
        if (file.node != node) continue;
        if (file.degraded) continue; // nothing was tracked, so nothing can be discarded

        // An fd the application already holds is reused only if it may write; a read-only one
        // would fail every pwrite and silently keep the un-synced bytes.
        int fd = -1;
        for (const auto& [open_fd, open_path] : fd_paths_) {
            if (open_path != path || !writable(open_fd)) continue;
            fd = open_fd;
            break;
        }
        const bool borrowed = fd >= 0;
        const uint64_t unsynced_count = static_cast<uint64_t>(file.unsynced.size());
        rollback(file, fd, path);
        // rollback() records storage.degraded when it cannot restore; do not also claim the
        // records were put back.
        if (!file.degraded) {
            record_storage(event_sink_, clock_, EventKind::storage_rollback, file.node,
                           EventPayload{.subject = file.id,
                                        .value = unsynced_count,
                                        .extra = static_cast<uint64_t>(file.durable_size),
                                        .amount_ns = 0});
        }
        if (fd < 0) {
            // The file is gone or unopenable, so its bytes cannot be un-written.
            drop_unsynced(file);
            file.last_sync.reset();
            continue;
        }

        if (!file.degraded && torn && file.last_sync.has_value() && file.last_sync->length > 0) {
            // An appended sector has no older bytes to fall back to, so tear its tail with zeros.
            const size_t tear = std::min(kTornSectorBytes, file.last_sync->length);
            const std::vector<char> zeros(tear, '\0');
            const off_t at =
                file.last_sync->offset + static_cast<off_t>(file.last_sync->length - tear);
            if (host_pwrite(fd, zeros.data(), zeros.size(), at) !=
                static_cast<ssize_t>(zeros.size())) {
                degraded_ = true;
                file.degraded = true;
                record_storage(event_sink_, clock_, EventKind::storage_degraded, file.node,
                               EventPayload{.subject = file.id});
            } else {
                record_storage(event_sink_, clock_, EventKind::storage_torn, file.node,
                               EventPayload{.subject = file.id,
                                            .value = static_cast<uint64_t>(tear),
                                            .amount_ns = static_cast<int64_t>(at)});
            }
        }

        if (!borrowed) (void)close(fd);
        drop_unsynced(file);
        file.last_sync.reset();
    }
}

void StorageModel::rollback(FileState& file, int& fd, const std::string& path) {
    if (fd < 0) fd = openat(AT_FDCWD, path.c_str(), O_RDWR);
    if (fd < 0) {
        // The model cannot restore this file; stop pretending it can.
        degraded_ = true;
        file.degraded = true;
        record_storage(event_sink_, clock_, EventKind::storage_degraded, file.node,
                       EventPayload{.subject = file.id});
        return;
    }

    // Newest first, so overlapping writes leave the oldest pre-image in place.
    for (auto record = file.unsynced.rbegin(); record != file.unsynced.rend(); ++record) {
        if (record->previous.empty()) continue;
        if (host_pwrite(fd, record->previous.data(), record->previous.size(), record->offset) !=
            static_cast<ssize_t>(record->previous.size())) {
            // A partial restore is not the state the crash produced.
            degraded_ = true;
            file.degraded = true;
            record_storage(event_sink_, clock_, EventKind::storage_degraded, file.node,
                           EventPayload{.subject = file.id});
            return;
        }
    }
    // Drops bytes appended past the last fsync, including zero-filled holes.
    if (host_ftruncate(fd, file.durable_size) != 0) {
        degraded_ = true;
        file.degraded = true;
        record_storage(event_sink_, clock_, EventKind::storage_degraded, file.node,
                       EventPayload{.subject = file.id});
    }
}

void StorageModel::drop_unsynced(FileState& file) {
    for (const WriteRecord& record : file.unsynced)
        tracked_bytes_ -= record.previous.size();
    file.unsynced.clear();
}

std::vector<std::string> StorageModel::tracked_paths() const {
    std::vector<std::pair<uint64_t, std::string>> rows;
    rows.reserve(files_.size());
    for (const auto& [path, file] : files_)
        rows.emplace_back(file.id, path);
    std::sort(rows.begin(), rows.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    std::vector<std::string> paths;
    paths.reserve(rows.size());
    for (auto& row : rows)
        paths.push_back(std::move(row.second));
    return paths;
}

std::vector<FileInfo> StorageModel::file_infos() const {
    std::vector<FileInfo> infos;
    infos.reserve(files_.size());
    for (const auto& [path, file] : files_) {
        uint64_t open_fds = 0;
        for (const auto& [fd, fd_path] : fd_paths_) {
            (void)fd;
            if (fd_path == path) ++open_fds;
        }
        auto display = display_paths_.find(path);
        const std::string shown_path = display == display_paths_.end() ? path : display->second;
        uint64_t unsynced_bytes = 0;
        for (const WriteRecord& record : file.unsynced)
            unsynced_bytes += record.previous.size();
        infos.push_back(FileInfo{
            file.id,
            shown_path,
            file.node,
            static_cast<int64_t>(file.durable_size),
            static_cast<uint64_t>(file.unsynced.size()),
            unsynced_bytes,
            file.last_sync.has_value() ? static_cast<int64_t>(file.last_sync->offset) : 0,
            file.last_sync.has_value() ? static_cast<uint64_t>(file.last_sync->length) : 0,
            file.degraded,
            open_fds,
        });
    }
    std::sort(infos.begin(), infos.end(),
              [](const FileInfo& left, const FileInfo& right) { return left.id < right.id; });
    return infos;
}

std::vector<OpenDescriptorInfo> StorageModel::open_descriptors() const {
    std::vector<OpenDescriptorInfo> descriptors;
    descriptors.reserve(fd_paths_.size());
    for (const auto& [fd, path] : fd_paths_) {
        const off_t offset = ::lseek(fd, 0, SEEK_CUR);
        const int flags = ::fcntl(fd, F_GETFL);
        const int fd_flags = ::fcntl(fd, F_GETFD);
        if (offset < 0 || flags < 0 || fd_flags < 0) continue;
        descriptors.push_back(OpenDescriptorInfo{fd, offset, flags, fd_flags, path});
    }
    std::sort(descriptors.begin(), descriptors.end(),
              [](const OpenDescriptorInfo& left, const OpenDescriptorInfo& right) {
                  return left.fd < right.fd;
              });
    return descriptors;
}

namespace {
std::string canonicalize_path(std::string_view path) {
    std::error_code error;
    const std::filesystem::path requested(path);
    const std::filesystem::path absolute = std::filesystem::absolute(requested, error);
    if (error) return std::string(path);
    return absolute.lexically_normal().string();
}

std::string private_name(const std::string& root, uint64_t id, std::string_view path) {
    const std::filesystem::path original(path);
    const std::string filename =
        original.filename().empty() ? "file" : original.filename().string();
    return root + "/new-" + std::to_string(id) + "-" + filename;
}
} // namespace

void StorageModel::begin_branch_storage(std::string root,
                                        const std::map<std::string, std::string>& redirects) {
    branch_root_ = std::move(root);
    branch_active_ = true;
    std::error_code error;
    std::filesystem::create_directories(branch_root_, error);
    for (const auto& [source, private_path] : redirects) {
        (void)private_path;
        captured_paths_.insert(source);
        captured_paths_.insert(canonicalize_path(source));
    }
    redirect_paths(redirects);
}

std::string StorageModel::redirect_path(std::string_view path, bool create) {
    if (!branch_active_) return std::string(path);
    const std::string canonical = canonicalize_path(path);
    auto exact = redirects_.find(std::string(path));
    if (exact != redirects_.end() && !exact->second.empty()) return exact->second;
    auto normalized = redirects_.find(canonical);
    if (normalized != redirects_.end() && !normalized->second.empty()) {
        return normalized->second;
    }
    if (create) {
        // An earlier read miss cached the absence; O_CREAT is allowed to create a private file.
        const bool same =
            exact != redirects_.end() && normalized != redirects_.end() && exact == normalized;
        if (exact != redirects_.end()) redirects_.erase(exact);
        if (normalized != redirects_.end() && !same) redirects_.erase(normalized);
    }

    std::error_code error;
    const std::filesystem::path canonical_path(canonical);
    const bool parent_exists =
        std::filesystem::is_directory(canonical_path.parent_path(), error) && !error;
    error.clear();
    const bool is_directory = std::filesystem::is_directory(canonical_path, error) && !error;
    if (!parent_exists || is_directory) {
        // A new path whose real parent does not exist must still fail with ENOENT in the branch.
        redirects_[std::string(path)].clear();
        redirects_[canonical].clear();
        return {};
    }
    const bool exists_on_host = std::filesystem::is_regular_file(canonical, error) && !error;
    if (exists_on_host && !captured_paths_.contains(canonical) && !create) {
        // The parent created or modified this path after the snapshot. Reading live bytes here
        // would make the branch a function of parent progress rather than the captured image.
        redirects_[std::string(path)].clear();
        redirects_[canonical].clear();
        return {};
    }
    const std::string private_path = private_name(branch_root_, next_new_file_++, path);
    redirects_[std::string(path)] = private_path;
    redirects_[canonical] = private_path;
    display_paths_[private_path] = std::string(path);
    const bool captured = captured_paths_.contains(canonical);
    captured_paths_.insert(canonical);
    if (exists_on_host && captured) {
        // A path present at capture is copied; a path created in this branch stays empty until the
        // application writes it. A post-capture parent path is not copied here.
        std::filesystem::copy_file(canonical, private_path,
                                   std::filesystem::copy_options::overwrite_existing, error);
    }
    return private_path;
}

std::string StorageModel::canonical_path(std::string_view path) const {
    if (!branch_active_) return std::string(path);
    const std::string canonical = canonicalize_path(path);
    auto entry = redirects_.find(canonical);
    if (entry != redirects_.end()) return entry->second;
    entry = redirects_.find(std::string(path));
    return entry == redirects_.end() ? canonical : entry->second;
}

void StorageModel::redirect_paths(const std::map<std::string, std::string>& redirects) {
    if (redirects.empty()) return;
    std::map<std::string, FileState> redirected;
    for (auto& [path, file] : files_) {
        auto entry = redirects.find(path);
        const std::string& target = entry == redirects.end() ? path : entry->second;
        redirected.emplace(target, std::move(file));
    }
    files_.swap(redirected);
    for (auto& [fd, path] : fd_paths_) {
        (void)fd;
        auto entry = redirects.find(path);
        if (entry != redirects.end()) path = entry->second;
    }
    for (const auto& [source, private_path] : redirects) {
        redirects_[source] = private_path;
        auto shown = display_paths_.find(source);
        display_paths_[private_path] = shown == display_paths_.end() ? source : shown->second;
    }
}

} // namespace cosmos

#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <stdarg.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

// Storage translation (docs/design.md §3, docs/fault-injection.md §8.2). The decision precedes the
// real call, so a faulted write leaves the file untouched and a faulted open creates nothing.

namespace {

// Bytes a write is about to replace, plus where to record them. offset < 0 means the pre-image
// could not be read, so the write must not be recorded.
struct PreImage {
    std::string bytes;
    off_t offset = -1;
};

// Captures before the real call. `at` < 0 means the stream position. O_APPEND wins over both the
// position and pwrite's offset, because the kernel appends anyway. Never leaks errno into a
// succeeding write.
PreImage capture_pre_image(cosmos::StorageModel& storage, int fd, off_t at, size_t length) {
    PreImage image;
    // An untracked fd has nothing to restore: skip the syscalls and the allocation entirely.
    if (!storage.is_tracked(fd)) return image;

    const int saved_errno = errno;
    struct stat st{};
    const int open_flags = fcntl(fd, F_GETFL);
    off_t offset = at >= 0 ? at : lseek(fd, 0, SEEK_CUR);
    if (offset >= 0 && fstat(fd, &st) == 0) {
        if ((open_flags & O_APPEND) != 0) offset = st.st_size;
        if (st.st_size > offset) {
            const size_t available = static_cast<size_t>(st.st_size - offset);
            image.bytes.resize(std::min(length, available));
            ssize_t got = pread(fd, image.bytes.data(), image.bytes.size(), offset);
            if (got < 0 && errno == EBADF) {
                // A write-only fd cannot pread: borrow a transient read handle on it.
                const std::string proc_path = "/proc/self/fd/" + std::to_string(fd);
                const int reader = openat(AT_FDCWD, proc_path.c_str(), O_RDONLY);
                if (reader >= 0) {
                    got = pread(reader, image.bytes.data(), image.bytes.size(), offset);
                    (void)close(reader);
                }
            }
            if (got < 0) {
                image.bytes.clear();
                errno = saved_errno;
                return image; // Without the pre-image the write must not be recorded.
            }
            image.bytes.resize(static_cast<size_t>(got));
        }
        image.offset = offset;
    }
    errno = saved_errno;
    return image;
}

// Records one successful real write: only what the kernel reported moved, and only the matching
// pre-image prefix, which a native short write leaves untouched.
void record_success(cosmos::StorageModel& storage, int fd, const PreImage& image, ssize_t ret,
                    cosmos::NodeId node) {
    if (ret <= 0 || image.offset < 0) return;
    const size_t keep = std::min(image.bytes.size(), static_cast<size_t>(ret));
    storage.record_write(fd, image.offset, std::string_view(image.bytes).substr(0, keep),
                         static_cast<size_t>(ret), node);
}

// Bytes a writev transfer covers. A null iovec transfers nothing, which keeps it ineligible.
size_t iov_total(const struct iovec* iov, int count) {
    size_t total = 0;
    for (int i = 0; iov != nullptr && i < count; ++i)
        total += iov[i].iov_len;
    return total;
}

// Leading entry count summing to exactly `bytes`, or -1 when no entry boundary lands there: a cut
// inside an entry has no legal short observable, so the caller keeps the whole transfer.
int iov_prefix(const struct iovec* iov, int count, size_t bytes) {
    size_t seen = 0;
    for (int i = 0; i <= count; ++i) {
        if (seen == bytes) return i;
        if (i < count) seen += iov[i].iov_len;
    }
    return -1;
}

// The bytes a write-family call may really transfer. ShortWrite means half, which only degrades to
// the full length when the request is 1 byte and has no legal short observable.
size_t write_request(cosmos::FaultKind kind, size_t length) {
    return kind == cosmos::FaultKind::ShortWrite && length >= 2 ? length / 2 : length;
}

// The errno a write-site or fsync-site fault answers with, or 0 when the real call goes through.
int storage_fault_errno(cosmos::FaultKind kind) {
    switch (kind) {
    case cosmos::FaultKind::WriteEio:
    case cosmos::FaultKind::FsyncEio:
        return EIO;
    case cosmos::FaultKind::NoSpace:
        return ENOSPC;
    default:
        return 0; // None, or a kind this site cannot emit.
    }
}

// Registers a tracked file under the opening fiber's node, so a reboot targets its owner.
void track_open_for_node(cosmos::Simulator* sim, int fd, const char* path, int flags) {
    // A reader must not steal ownership from the node that writes the file.
    if ((flags & O_ACCMODE) != O_RDONLY) sim->storage().set_active_node(sim->current_node());
    sim->storage().track_open(fd, path, (flags & O_TRUNC) != 0);
}

// One storage producer. Called after the wrapped call has decided its result; recording cannot
// change the file, errno or the durability model.
void record_storage_event(cosmos::Simulator* sim, cosmos::EventKind kind, int fd,
                          cosmos::EventPayload payload) {
    payload.subject = static_cast<uint64_t>(fd);
    cosmos::wrappers::record_event(sim, kind, payload);
}

// A successful open can land on fd 0 after the application closed stdin. The descriptor is then
// the application's file, not the harness stream, so stdin ownership must be released first.
void note_open_result(cosmos::Simulator* sim, int fd, const char* path, int flags) {
    if (fd < 0) return;
    if (fd == STDIN_FILENO) sim->release_stdin();
    if (cosmos::wrappers::storage_fd_eligible(fd)) track_open_for_node(sim, fd, path, flags);
}

} // namespace

extern "C" {

int __real_open(const char* pathname, int flags, ...);
int __real_open64(const char* pathname, int flags, ...);
int __real___open_2(const char* pathname, int flags);
int __real___open64_2(const char* pathname, int flags);
ssize_t __real_read(int fd, void* buf, size_t count);
ssize_t __real___read_chk(int fd, void* buf, size_t count, size_t buflen);
ssize_t __real_write(int fd, const void* buf, size_t count);
// Fortify's failure path; __write_chk is not exported by glibc, but this is.
void __chk_fail(void) __attribute__((noreturn));
ssize_t __real_pwrite(int fd, const void* buf, size_t count, off_t offset);
ssize_t __real_pwrite64(int fd, const void* buf, size_t count, off64_t offset);
ssize_t __real_writev(int fd, const struct iovec* iov, int count);
ssize_t __real_pwritev(int fd, const struct iovec* iov, int count, off_t offset);
int __real_fsync(int fd);
int __real_fdatasync(int fd);
int __real_ftruncate(int fd, off_t length);
int __real_ftruncate64(int fd, off64_t length);

// The va_arg must be read before anything else, or the va_list use is invalid.
int __wrap_open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
#if defined(O_TMPFILE)
    const bool mode_needed = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
#else
    const bool mode_needed = (flags & O_CREAT) != 0;
#endif
    if (mode_needed) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_open(pathname, flags, mode);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break; // None, or an unknown kind: the real call goes through unchanged.
    }
    // Only a successful open registers: a faulted open returned above and creates nothing.
    const int fd = __real_open(pathname, flags, mode);
    note_open_result(sim, fd, pathname, flags);
    if (fd >= 0) {
        record_storage_event(sim, cosmos::EventKind::storage_open, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(flags)});
    }
    return fd;
}

// glibc substitutes __open_2/__open64_2 for open() when the call has no mode argument and
// _FORTIFY_SOURCE is on; --wrap=open does not see those symbols, so without this a fortified
// build bypasses the site silently (the same hole __read_chk and __recv_chk were).
// open64 is a separate symbol with the same site: an explicit open64() must not bypass it.
int __wrap_open64(const char* pathname, int flags, ...) {
    mode_t mode = 0;
#if defined(O_TMPFILE)
    const bool mode_needed = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
#else
    const bool mode_needed = (flags & O_CREAT) != 0;
#endif
    if (mode_needed) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_open64(pathname, flags, mode);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break; // None, or an unknown kind: the real call goes through unchanged.
    }
    const int fd = __real_open64(pathname, flags, mode);
    note_open_result(sim, fd, pathname, flags);
    if (fd >= 0) {
        record_storage_event(sim, cosmos::EventKind::storage_open, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(flags)});
    }
    return fd;
}

int __wrap___open_2(const char* pathname, int flags) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real___open_2(pathname, flags);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break;
    }
    const int fd = __real___open_2(pathname, flags);
    note_open_result(sim, fd, pathname, flags);
    if (fd >= 0) {
        record_storage_event(sim, cosmos::EventKind::storage_open, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(flags)});
    }
    return fd;
}

int __wrap___open64_2(const char* pathname, int flags) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real___open64_2(pathname, flags);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::open)) {
    case cosmos::FaultKind::OpenEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break;
    }
    const int fd = __real___open64_2(pathname, flags);
    note_open_result(sim, fd, pathname, flags);
    if (fd >= 0) {
        record_storage_event(sim, cosmos::EventKind::storage_open, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(flags)});
    }
    return fd;
}

ssize_t __wrap_read(int fd, void* buf, size_t count) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_read(fd, buf, count);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    // When the universe has installed a stdin buffer, fd 0 is the harness stream and never the
    // host's stdin. Without set_stdin the host descriptor is used as-is.
    if (cosmos::wrappers::owns_stdin(sim, fd)) {
        return sim->read_stdin(buf, count);
    }

    if (!cosmos::wrappers::storage_read_eligible(fd, count)) {
        return __real_read(fd, buf, count);
    }

    // Defense in depth: a virtual socket fd must never spend a Storage draw, even if the
    // static range check ever drifts. The transport has no read/write path, so it reaches
    // the host (EBADF on a kernel-unknown number) unfaulted.
    if (sim->net().state(fd).known) {
        return __real_read(fd, buf, count);
    }
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::read)) {
    case cosmos::FaultKind::ReadEio:
        errno = EIO;
        record_storage_event(sim, cosmos::EventKind::storage_read, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                                  .extra = static_cast<uint64_t>(EIO)});
        return -1;
    default:
        break;
    }
    const ssize_t ret = __real_read(fd, buf, count);
    record_storage_event(sim, cosmos::EventKind::storage_read, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

// glibc substitutes __read_chk for read() when the destination size is known; --wrap=read does not
// see that symbol, so without this the site is silently skipped in such builds.
ssize_t __wrap___read_chk(int fd, void* buf, size_t count, size_t buflen) {
    if (count > buflen) __chk_fail(); // what __read_chk itself does before reading
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real___read_chk(fd, buf, count, buflen);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    if (cosmos::wrappers::owns_stdin(sim, fd)) {
        return sim->read_stdin(buf, count);
    }

    if (!cosmos::wrappers::storage_read_eligible(fd, count)) {
        return __real___read_chk(fd, buf, count, buflen);
    }

    if (sim->net().state(fd).known) {
        return __real___read_chk(fd, buf, count, buflen);
    }
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::read)) {
    case cosmos::FaultKind::ReadEio:
        errno = EIO;
        record_storage_event(sim, cosmos::EventKind::storage_read, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                                  .extra = static_cast<uint64_t>(EIO)});
        return -1;
    default:
        break;
    }
    const ssize_t ret = __real___read_chk(fd, buf, count, buflen);
    record_storage_event(sim, cosmos::EventKind::storage_read, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

ssize_t __wrap_write(int fd, const void* buf, size_t count) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_write(fd, buf, count);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_write_eligible(fd, count)) {
        return __real_write(fd, buf, count);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_write(fd, buf, count);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_write, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                                  .extra = static_cast<uint64_t>(error)});
        return -1;
    }

    // A faulted call has already returned, so only a real write captures the bytes it replaces.
    const PreImage image = capture_pre_image(sim->storage(), fd, -1, count);
    const ssize_t ret = __real_write(fd, buf, write_request(kind, count));
    record_success(sim->storage(), fd, image, ret, sim->current_node());
    record_storage_event(sim, cosmos::EventKind::storage_write, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

// The fortified alias of write. glibc fortifies write with a compiler builtin and exports no
// __write_chk symbol, so the bounds check is repeated here and the transfer goes through write.
ssize_t __wrap___write_chk(int fd, const void* buf, size_t count, size_t buflen) {
    if (count > buflen) __chk_fail(); // what __write_chk itself does before writing
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_write(fd, buf, count);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_write_eligible(fd, count)) {
        return __real_write(fd, buf, count);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_write(fd, buf, count);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_write, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                                  .extra = static_cast<uint64_t>(error)});
        return -1;
    }

    const PreImage image = capture_pre_image(sim->storage(), fd, -1, count);
    const ssize_t ret = __real_write(fd, buf, write_request(kind, count));
    record_success(sim->storage(), fd, image, ret, sim->current_node());
    record_storage_event(sim, cosmos::EventKind::storage_write, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

// The positional form of write: same site, same decision, but the pre-image comes from the given
// offset rather than the stream position (both of which O_APPEND overrides).
ssize_t __wrap_pwrite(int fd, const void* buf, size_t count, off_t offset) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pwrite(fd, buf, count, offset);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_write_eligible(fd, count)) {
        return __real_pwrite(fd, buf, count, offset);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_pwrite(fd, buf, count, offset);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_pwrite, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                                  .extra = static_cast<uint64_t>(error),
                                                  .amount_ns = static_cast<int64_t>(offset)});
        return -1;
    }

    const PreImage image = capture_pre_image(sim->storage(), fd, offset, count);
    const ssize_t ret = __real_pwrite(fd, buf, write_request(kind, count), offset);
    record_success(sim->storage(), fd, image, ret, sim->current_node());
    record_storage_event(sim, cosmos::EventKind::storage_pwrite, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret),
                                              .amount_ns = static_cast<int64_t>(offset)});
    return ret;
}

ssize_t __wrap_pwrite64(int fd, const void* buf, size_t count, off64_t offset) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pwrite64(fd, buf, count, offset);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_write_eligible(fd, count)) {
        return __real_pwrite64(fd, buf, count, offset);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_pwrite64(fd, buf, count, offset);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_pwrite, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                                  .extra = static_cast<uint64_t>(error),
                                                  .amount_ns = static_cast<int64_t>(offset)});
        return -1;
    }

    const PreImage image = capture_pre_image(sim->storage(), fd, static_cast<off_t>(offset), count);
    const ssize_t ret = __real_pwrite64(fd, buf, write_request(kind, count), offset);
    record_success(sim->storage(), fd, image, ret, sim->current_node());
    record_storage_event(sim, cosmos::EventKind::storage_pwrite, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(count),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret),
                                              .amount_ns = static_cast<int64_t>(offset)});
    return ret;
}

// A gather write is one call for the model: the pre-image covers the whole span the iovec may
// touch, and the record keeps whatever the kernel reported.
ssize_t __wrap_writev(int fd, const struct iovec* iov, int count) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_writev(fd, iov, count);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    const size_t total = iov_total(iov, count);
    if (!cosmos::wrappers::storage_write_eligible(fd, total)) {
        return __real_writev(fd, iov, count);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_writev(fd, iov, count);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_writev, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(total),
                                                  .extra = static_cast<uint64_t>(error)});
        return -1;
    }

    const PreImage image = capture_pre_image(sim->storage(), fd, -1, total);
    ssize_t ret;
    const size_t request = write_request(kind, total);
    if (request == total) {
        ret = __real_writev(fd, iov, count);
    } else if (request <= iov[0].iov_len) {
        // The cut lies inside the first entry, so one plain write transfers exactly that prefix.
        ret = __real_write(fd, iov[0].iov_base, request);
    } else if (const int prefix = iov_prefix(iov, count, request); prefix >= 0) {
        ret = __real_writev(fd, iov, prefix);
    } else {
        // No boundary at this cut: a short transfer is not observable, so the call stays whole.
        ret = __real_writev(fd, iov, count);
    }
    record_success(sim->storage(), fd, image, ret, sim->current_node());
    record_storage_event(sim, cosmos::EventKind::storage_writev, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(total),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

ssize_t __wrap_pwritev(int fd, const struct iovec* iov, int count, off_t offset) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_pwritev(fd, iov, count, offset);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    const size_t total = iov_total(iov, count);
    if (!cosmos::wrappers::storage_write_eligible(fd, total)) {
        return __real_pwritev(fd, iov, count, offset);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_pwritev(fd, iov, count, offset);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_writev, fd,
                             cosmos::EventPayload{.value = static_cast<uint64_t>(total),
                                                  .extra = static_cast<uint64_t>(error),
                                                  .amount_ns = static_cast<int64_t>(offset)});
        return -1;
    }

    const PreImage image = capture_pre_image(sim->storage(), fd, offset, total);
    ssize_t ret;
    const size_t request = write_request(kind, total);
    if (request == total) {
        ret = __real_pwritev(fd, iov, count, offset);
    } else if (request <= iov[0].iov_len) {
        ret = __real_pwrite(fd, iov[0].iov_base, request, offset);
    } else if (const int prefix = iov_prefix(iov, count, request); prefix >= 0) {
        ret = __real_pwritev(fd, iov, prefix, offset);
    } else {
        ret = __real_pwritev(fd, iov, count, offset);
    }
    record_success(sim->storage(), fd, image, ret, sim->current_node());
    record_storage_event(sim, cosmos::EventKind::storage_writev, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(total),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret),
                                              .amount_ns = static_cast<int64_t>(offset)});
    return ret;
}

int __wrap_fsync(int fd) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_fsync(fd);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_fd_eligible(fd)) {
        return __real_fsync(fd);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_fsync(fd);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::fsync);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_fsync, fd,
                             cosmos::EventPayload{.extra = static_cast<uint64_t>(error)});
        return -1;
    }
    const int ret = __real_fsync(fd);
    // Only a real, successful fsync makes the current size durable; a faulted one commits nothing.
    if (ret == 0) {
        sim->storage().commit_fsync(fd);
    }
    record_storage_event(sim, cosmos::EventKind::storage_fsync, fd,
                         cosmos::EventPayload{.extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

// fdatasync is the same durability boundary for an application that only needs the data: without
// this wrapper its bytes were never committed and a reboot rolled them back.
int __wrap_fdatasync(int fd) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_fdatasync(fd);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_fd_eligible(fd)) {
        return __real_fdatasync(fd);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_fdatasync(fd);
    }
    const cosmos::FaultKind kind =
        cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::fsync);
    if (const int error = storage_fault_errno(kind); error != 0) {
        errno = error;
        record_storage_event(sim, cosmos::EventKind::storage_fdatasync, fd,
                             cosmos::EventPayload{.extra = static_cast<uint64_t>(error)});
        return -1;
    }
    const int ret = __real_fdatasync(fd);
    if (ret == 0) {
        sim->storage().commit_fsync(fd);
    }
    record_storage_event(sim, cosmos::EventKind::storage_fdatasync, fd,
                         cosmos::EventPayload{.extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

// Truncation has no injectable outcome, so it is not a site and never reaches the injector. What
// it does need is a durability event: an application cut must survive a reboot.
int __wrap_ftruncate(int fd, off_t length) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_ftruncate(fd, length);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_fd_eligible(fd)) {
        return __real_ftruncate(fd, length);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_ftruncate(fd, length);
    }
    const int ret = __real_ftruncate(fd, length);
    if (ret == 0) {
        sim->storage().record_truncate(fd, static_cast<off_t>(length));
    }
    record_storage_event(sim, cosmos::EventKind::storage_ftruncate, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(length),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

int __wrap_ftruncate64(int fd, off64_t length) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_ftruncate64(fd, length);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_fd_eligible(fd)) {
        return __real_ftruncate64(fd, length);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->net().state(fd).known) {
        return __real_ftruncate64(fd, length);
    }
    const int ret = __real_ftruncate64(fd, length);
    if (ret == 0) {
        sim->storage().record_truncate(fd, static_cast<off_t>(length));
    }
    record_storage_event(sim, cosmos::EventKind::storage_ftruncate, fd,
                         cosmos::EventPayload{.value = static_cast<uint64_t>(length),
                                              .extra = static_cast<uint64_t>(ret < 0 ? 0 : ret)});
    return ret;
}

} // extern "C"

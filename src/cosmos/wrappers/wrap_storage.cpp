#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Storage wrapper translation (docs/design.md §3 Storage, docs/fault-injection.md §8.2):
//
// - Passthrough-first: without an active Simulator the host owns everything.
// - Decide-before-call: a decided failure (-1 + errno) returns without performing the real
//   call, so a faulted write leaves the file untouched and a faulted open creates nothing.
//   Faults can therefore fire on fds the real call would have failed with EBADF; every
//   returned result stays one the real API can produce (Rule 15).
// - Eligibility (wrapper_fault.hpp): standard stream fds and zero-length transfers never
//   reach the injector — logging must not consume Storage-stream draws, and no outcome is
//   observable on an empty transfer. Everything else is eligible, 1-byte writes included:
//   they can legally fail with EIO/ENOSPC.
// - ShortWrite is a shaped success, not an error: the wrapper performs a REAL partial transfer
//   of count / 2 bytes and returns what the real call returned. write() reporting k bytes must
//   mean those k bytes were transferred — claiming a short count without transferring would be
//   an impossible world (Rule 15). On a 1-byte write no legal short observable exists (0 bytes
//   invites retry loops, 1 byte is the whole request), so a fired ShortWrite there degrades to
//   a complete write: the fire is still counted, the observable is the best the API allows.

extern "C" {

int __real_open(const char* pathname, int flags, ...);
ssize_t __real_read(int fd, void* buf, size_t count);
ssize_t __real_write(int fd, const void* buf, size_t count);
int __real_fsync(int fd);

// open(2) is variadic: the optional mode argument is meaningful for O_CREAT and, on Linux,
// for O_TMPFILE. Read it before anything else so the va_list use stays correct, then decide.
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
    return __real_open(pathname, flags, mode);
}

ssize_t __wrap_read(int fd, void* buf, size_t count) {
    if (!cosmos::Simulator::has_current() || cosmos::wrappers::in_wrapper_logic) {
        return __real_read(fd, buf, count);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    if (!cosmos::wrappers::storage_read_eligible(fd, count)) {
        return __real_read(fd, buf, count);
    }

    auto* sim = cosmos::Simulator::current();
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::read)) {
    case cosmos::FaultKind::ReadEio:
        errno = EIO;
        return -1;
    default:
        break;
    }
    return __real_read(fd, buf, count);
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
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::write)) {
    case cosmos::FaultKind::WriteEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    case cosmos::FaultKind::ShortWrite:
        // count >= 2: a real partial transfer of the decided count; a nested short result or
        // an error from the real call is returned as-is, so the observable stays honest.
        if (count >= 2) {
            return __real_write(fd, buf, count / 2);
        }
        // Degenerate 1-byte case: no legal short observable exists; degrade to the complete
        // write (see the file comment). The fire stays counted on the injector side.
        return __real_write(fd, buf, count);
    default:
        break;
    }
    return __real_write(fd, buf, count);
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
    switch (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Storage, cosmos::SiteId::fsync)) {
    case cosmos::FaultKind::FsyncEio:
        errno = EIO;
        return -1;
    case cosmos::FaultKind::NoSpace:
        errno = ENOSPC;
        return -1;
    default:
        break;
    }
    return __real_fsync(fd);
}

} // extern "C"

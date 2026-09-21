#include "cosmos/cosmos.hpp"
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

// Linker-wrapping fail-fast stubs for the POSIX network socket surface (docs/design.md §3
// "Network"). The in-process simulated topology (latency/loss/reorder/partition faults) does
// not exist yet, and a deterministic universe must not touch real kernel sockets: real
// sockets read host state the seed does not determine, and a blocking recv/accept parks the
// one OS thread every fiber shares, hanging the universe with no diagnostic. So each
// socket-specific wrapper aborts with the reason while a universe is current, and passes
// through without one — host code (harness, logging) keeps its sockets.
//
// close() is the exception: it stays passthrough under a universe too, because descriptors
// are shared with the implemented storage surface and an fd's kind cannot be distinguished
// here.

namespace {

[[noreturn]] void abort_unimplemented(const char* call) {
    const char prefix[] = "cosmos: simulated network is not implemented yet: ";
    const char suffix[] =
        "() called with an active universe; refusing real kernel sockets (do not wrap the "
        "socket symbols, or keep socket APIs out of simulation builds until it lands)\n";
    (void)::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    (void)::write(STDERR_FILENO, call, strlen(call));
    (void)::write(STDERR_FILENO, suffix, sizeof(suffix) - 1);
    ::abort();
}

} // namespace

extern "C" {

int __real_socket(int domain, int type, int protocol);
int __real_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int __real_listen(int sockfd, int backlog);
int __real_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int __real_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t __real_send(int sockfd, const void* buf, size_t len, int flags);
ssize_t __real_recv(int sockfd, void* buf, size_t len, int flags);
int __real_close(int fd);

int __wrap_socket(int domain, int type, int protocol) {
    if (!cosmos::Simulator::has_current()) {
        return __real_socket(domain, type, protocol);
    }
    abort_unimplemented("socket");
}

int __wrap_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    if (!cosmos::Simulator::has_current()) {
        return __real_bind(sockfd, addr, addrlen);
    }
    abort_unimplemented("bind");
}

int __wrap_listen(int sockfd, int backlog) {
    if (!cosmos::Simulator::has_current()) {
        return __real_listen(sockfd, backlog);
    }
    abort_unimplemented("listen");
}

int __wrap_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    if (!cosmos::Simulator::has_current()) {
        return __real_accept(sockfd, addr, addrlen);
    }
    abort_unimplemented("accept");
}

int __wrap_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    if (!cosmos::Simulator::has_current()) {
        return __real_connect(sockfd, addr, addrlen);
    }
    abort_unimplemented("connect");
}

ssize_t __wrap_send(int sockfd, const void* buf, size_t len, int flags) {
    if (!cosmos::Simulator::has_current()) {
        return __real_send(sockfd, buf, len, flags);
    }
    abort_unimplemented("send");
}

ssize_t __wrap_recv(int sockfd, void* buf, size_t len, int flags) {
    if (!cosmos::Simulator::has_current()) {
        return __real_recv(sockfd, buf, len, flags);
    }
    abort_unimplemented("recv");
}

int __wrap_close(int fd) { return __real_close(fd); }

} // extern "C"

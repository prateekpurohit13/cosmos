#include "cosmos/cosmos.hpp"

#include <cassert>
#include <csignal>
#include <iostream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// Fail-fast contract of the socket stubs (src/cosmos/wrappers/wrap_net.cpp): with a universe
// current they abort rather than touch real kernel sockets from a deterministic
// single-threaded world; without one they pass through so host code keeps its sockets. The
// __wrap_* symbols are invoked directly (the __read_chk precedent), so nothing depends on
// which call sites the compiler emits; the link flags in CMake exist to resolve every
// __real_* reference in the pulled-in wrap_net.o.

extern "C" {
int __wrap_socket(int domain, int type, int protocol);
int __wrap_bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int __wrap_listen(int sockfd, int backlog);
int __wrap_accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
int __wrap_connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
ssize_t __wrap_send(int sockfd, const void* buf, size_t len, int flags);
ssize_t __wrap_recv(int sockfd, void* buf, size_t len, int flags);
int __wrap_close(int fd);
}

namespace {

void must(bool ok) { assert(ok); }

void test_passthrough_without_universe() {
    must(!cosmos::Simulator::has_current());
    // A plain call site routes here through --wrap=socket; the direct call below pins the
    // same path.
    const int plain_fd = socket(AF_INET, SOCK_STREAM, 0);
    must(plain_fd >= 0);
    must(__wrap_close(plain_fd) == 0);

    const int fd = __wrap_socket(AF_INET, SOCK_STREAM, 0);
    must(fd >= 0);
    must(__wrap_close(fd) == 0);
    std::cout << "[PASS] test_passthrough_without_universe" << std::endl;
}

// close() must stay host-side even under a universe: descriptors are shared with the
// implemented storage surface, and their kind cannot be told apart.
void test_close_stays_host_side_under_universe() {
    must(!cosmos::Simulator::has_current());
    const int fd = __wrap_socket(AF_INET, SOCK_STREAM, 0);
    must(fd >= 0);
    cosmos::Simulator sim(1);
    cosmos::Simulator::set_current(&sim);
    must(__wrap_close(fd) == 0);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_close_stays_host_side_under_universe" << std::endl;
}

// The arguments never matter: each wrapper aborts before touching them.
void call_socket() { (void)__wrap_socket(AF_INET, SOCK_STREAM, 0); }
void call_bind() { (void)__wrap_bind(-1, nullptr, 0); }
void call_listen() { (void)__wrap_listen(-1, 1); }
void call_accept() { (void)__wrap_accept(-1, nullptr, nullptr); }
void call_connect() { (void)__wrap_connect(-1, nullptr, 0); }
void call_send() { (void)__wrap_send(-1, nullptr, 0, 0); }
void call_recv() { (void)__wrap_recv(-1, nullptr, 0, 0); }

// The abort is the contract, so each case runs in a forked child and the parent observes
// SIGABRT. No asserts in the child: a failed assert there also raises SIGABRT and would be
// indistinguishable from the wrapper firing.
bool dies_with_sigabrt(void (*call)(void)) {
    const pid_t pid = fork();
    must(pid >= 0);
    if (pid == 0) {
        cosmos::Simulator sim(1);
        cosmos::Simulator::set_current(&sim);
        call();
        _exit(0); // Unreachable when the wrapper aborts as it must.
    }
    int status = 0;
    must(waitpid(pid, &status, 0) == pid);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

void test_socket_calls_abort_under_universe() {
    struct Case {
        const char* name;
        void (*call)(void);
    };
    const Case cases[] = {
        {"socket", call_socket}, {"bind", call_bind},       {"listen", call_listen},
        {"accept", call_accept}, {"connect", call_connect}, {"send", call_send},
        {"recv", call_recv},
    };
    for (const Case& c : cases) {
        must(dies_with_sigabrt(c.call));
        std::cout << "[PASS] abort under universe: " << c.name << std::endl;
    }
}

} // namespace

int main() {
    test_passthrough_without_universe();
    test_close_stays_host_side_under_universe();
    test_socket_calls_abort_under_universe();
    std::cout << "All network wrapper tests passed successfully!" << std::endl;
    return 0;
}

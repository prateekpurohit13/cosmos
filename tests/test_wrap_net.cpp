#include "net_test_support.hpp"

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>

extern "C" int __wrap___poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fdslen);

void test_passthrough_without_universe() {
    must(!cosmos::Simulator::has_current());
    const int plain = socket(AF_INET, SOCK_STREAM, 0);
    must(plain >= 0);
    must(close(plain) == 0);

    int sv[2] = {-1, -1};
    must(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    must(send(sv[0], "abc", 3, 0) == 3);
    char buf[4] = {};
    must(recv(sv[1], buf, sizeof(buf), 0) == 3);
    must(std::memcmp(buf, "abc", 3) == 0);
    must(close(sv[0]) == 0);
    must(close(sv[1]) == 0);
    std::cout << "[PASS] test_passthrough_without_universe" << std::endl;
}

std::string loopback_transcript(uint64_t seed) {
    cosmos::Simulator sim(seed);
    must(sim.install_faults(network_config(cosmos::SiteId::send, cosmos::FaultKind::PacketCorrupt))
             .has_value());
    cosmos::Simulator::set_current(&sim);

    Cluster pair = open_pair();
    char payload[64];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = static_cast<char>('A' + (i % 26));
    must(send(pair.client, payload, sizeof(payload), 0) == static_cast<ssize_t>(sizeof(payload)));

    struct pollfd pfd{pair.server, POLLIN, 0};
    must(poll(&pfd, 1, 0) == 1);
    must((pfd.revents & POLLIN) != 0);

    char got[64] = {};
    must(recv(pair.server, got, sizeof(got), 0) == static_cast<ssize_t>(sizeof(got)));
    drop_pair(pair);

    std::string out(got, sizeof(got));
    out += "|";
    out += std::to_string(sim.now().ns);
    cosmos::Simulator::set_current(nullptr);
    return out;
}

void test_loopback_is_byte_perfect_and_deterministic() {
    cosmos::Simulator sim(11);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    const char payload[] = "the quick brown fox";
    must(send(pair.client, payload, sizeof(payload) - 1, 0) ==
         static_cast<ssize_t>(sizeof(payload) - 1));
    char got[32] = {};
    must(recv(pair.server, got, sizeof(got), 0) == static_cast<ssize_t>(sizeof(payload) - 1));
    must(std::memcmp(got, payload, sizeof(payload) - 1) == 0);

    int sv[2] = {-1, -1};
    must(socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0);
    must(sv[0] >= cosmos::kVirtualFdBase && sv[1] >= cosmos::kVirtualFdBase);
    must(send(sv[1], "pair", 4, 0) == 4);
    char back[8] = {};
    must(recv(sv[0], back, sizeof(back), 0) == 4);
    must(std::memcmp(back, "pair", 4) == 0);
    must(close(sv[0]) == 0);
    must(close(sv[1]) == 0);

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);

    must(loopback_transcript(4242) == loopback_transcript(4242));
    std::cout << "[PASS] test_loopback_is_byte_perfect_and_deterministic" << std::endl;
}

void test_poll_readiness_and_chk_alias() {
    cosmos::Simulator sim(91);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    struct pollfd out{pair.client, POLLOUT, 0};
    must(poll(&out, 1, 0) == 1);
    must((out.revents & POLLOUT) != 0);

    struct pollfd in{pair.server, POLLIN, 0};
    must(poll(&in, 1, 0) == 0);

    must(send(pair.client, "ping", 4, 0) == 4);
    in.revents = 0;
    must(poll(&in, 1, 0) == 1);
    must((in.revents & POLLIN) != 0);

    in.revents = 0;
    must(__wrap___poll_chk(&in, 1, 0, sizeof(in)) == 1);
    must((in.revents & POLLIN) != 0);

    struct pollfd empty{pair.client, POLLIN, 0};
    const cosmos::Time before = sim.now();
    must(poll(&empty, 1, 10) == 0);
    must(sim.now() >= before + cosmos::Duration{10'000'000});

    drop_pair(pair);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_poll_readiness_and_chk_alias" << std::endl;
}

void test_nonblocking_paths_and_fcntl() {
    cosmos::Simulator sim(71);
    cosmos::Simulator::set_current(&sim);

    uint16_t port = 0;
    const int listener_fd = bind_listener(&port, /*nonblocking=*/true);
    Address peer;
    socklen_t len = peer.size();
    errno = 0;
    must(accept(listener_fd, peer.as_mutable_sockaddr(), &len) == -1);
    must(errno == EAGAIN);

    // The ordinary way an application turns a socket non-blocking, which the transport must honour.
    const int plain = socket(AF_INET, SOCK_STREAM, 0);
    must(fcntl(plain, F_GETFL) == O_RDWR);
    must(fcntl(plain, F_SETFL, O_NONBLOCK) == 0);
    must((fcntl(plain, F_GETFL) & O_NONBLOCK) != 0);

    Cluster pair = open_pair();
    errno = 0;
    char buf[4] = {};
    must(recv(pair.server, buf, sizeof(buf), MSG_DONTWAIT) == -1);
    must(errno == EAGAIN);

    close(plain);
    drop_pair(pair);
    close(listener_fd);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_nonblocking_paths_and_fcntl" << std::endl;
}

void test_status_errors_and_close_semantics() {
    cosmos::Simulator sim(81);
    cosmos::Simulator::set_current(&sim);
    Cluster pair = open_pair();

    errno = 0;
    must(send(pair.listener, "x", 1, 0) == -1);
    must(errno == ENOTCONN);
    char probe[4] = {};
    errno = 0;
    must(recv(pair.listener, probe, sizeof(probe), MSG_DONTWAIT) == -1);
    must(errno == ENOTCONN);
    errno = 0;
    must(send(5000, "x", 1, 0) == -1);
    must(errno == EBADF);

    must(close(pair.server) == 0);
    pair.server = -1;
    errno = 0;
    char buf[4] = {};
    must(recv(pair.client, buf, sizeof(buf), MSG_DONTWAIT) == 0);

    must(shutdown(pair.client, SHUT_WR) == 0);
    must(close(pair.client) == 0);
    pair.client = -1;
    must(close(pair.listener) == 0);
    pair.listener = -1;
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_status_errors_and_close_semantics" << std::endl;
}

int main() {
    test_passthrough_without_universe();
    test_loopback_is_byte_perfect_and_deterministic();
    test_poll_readiness_and_chk_alias();
    test_nonblocking_paths_and_fcntl();
    test_status_errors_and_close_semantics();
    std::cout << "All network transport tests passed successfully!" << std::endl;
    return 0;
}

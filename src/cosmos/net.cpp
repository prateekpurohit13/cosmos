#include "cosmos/net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace cosmos {

namespace {

void write_addr(struct sockaddr* addr, socklen_t* addrlen, uint16_t port) {
    if (addr == nullptr || addrlen == nullptr) return;
    struct sockaddr_in full{};
    full.sin_family = AF_INET;
    full.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    full.sin_port = htons(port);
    const socklen_t available = *addrlen;
    *addrlen = sizeof(full);
    if (available >= static_cast<socklen_t>(sizeof(full))) {
        std::memcpy(addr, &full, sizeof(full));
        return;
    }
    std::memcpy(addr, &full, static_cast<size_t>(available));
}

} // namespace

Net::Net(Scheduler& scheduler, VirtualClock& clock) : scheduler_(scheduler), clock_(clock) {}

Net::Endpoint* Net::get(int fd) {
    const int index = fd - kVirtualFdBase;
    if (index < 0 || static_cast<size_t>(index) >= kMaxEndpoints) return nullptr;
    Endpoint& ep = endpoints_[static_cast<size_t>(index)];
    return ep.used ? &ep : nullptr;
}

const Net::Endpoint* Net::get(int fd) const { return const_cast<Net*>(this)->get(fd); }

EndpointState Net::state(int fd) const {
    EndpointState out;
    const Endpoint* ep = get(fd);
    if (ep == nullptr) return out;
    out.known = true;
    out.listener = ep->listener;
    out.connected = ep->connected;
    out.pending = !ep->pending.empty();
    out.peer_closed = ep->peer_closed;
    out.nonblocking = ep->nonblocking;
    return out;
}

int Net::create_endpoint() {
    for (size_t i = 0; i < kMaxEndpoints; ++i) {
        Endpoint& ep = endpoints_[i];
        if (ep.used) continue;
        ep = Endpoint{};
        ep.used = true;
        return kVirtualFdBase + static_cast<int>(i);
    }
    errno = EMFILE;
    return -1;
}

int Net::socket(int domain, int type, int protocol) {
    if (domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if ((type & SOCK_STREAM) != SOCK_STREAM) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (protocol != 0 && protocol != IPPROTO_TCP) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    const int fd = create_endpoint();
    if (fd < 0) return -1;
    get(fd)->nonblocking = (type & SOCK_NONBLOCK) != 0;
    return fd;
}

int Net::pair(int domain, int type, int protocol, int out[2]) {
    if (domain != AF_UNIX && domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if ((type & SOCK_STREAM) != SOCK_STREAM || protocol != 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    const int left = create_endpoint();
    if (left < 0) return -1;
    const int right = create_endpoint();
    if (right < 0) {
        release(*get(left));
        return -1;
    }
    Endpoint& a = *get(left);
    Endpoint& b = *get(right);
    a.connected = true;
    b.connected = true;
    a.nonblocking = (type & SOCK_NONBLOCK) != 0;
    b.nonblocking = a.nonblocking;
    a.peer = right - kVirtualFdBase;
    b.peer = left - kVirtualFdBase;
    a.port = next_port_++;
    b.port = next_port_++;
    signal_change();
    out[0] = left;
    out[1] = right;
    return 0;
}

int Net::bind(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (ep->connected) {
        errno = EINVAL;
        return -1;
    }
    if (addr == nullptr || addrlen < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    if (addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (ep->port != 0) {
        errno = EINVAL;
        return -1;
    }
    const uint16_t requested = ntohs(reinterpret_cast<const struct sockaddr_in*>(addr)->sin_port);
    if (requested != 0 && find_listener(requested) != nullptr) {
        errno = EADDRINUSE;
        return -1;
    }
    ep->port = requested == 0 ? next_port_++ : requested;
    return 0;
}

int Net::listen(int fd, int backlog) {
    (void)backlog;
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (ep->connected) {
        errno = EINVAL;
        return -1;
    }
    ep->listener = true;
    return 0;
}

Net::Endpoint* Net::find_listener(uint16_t port) {
    for (Endpoint& ep : endpoints_) {
        if (ep.used && ep.listener && ep.port == port) return &ep;
    }
    return nullptr;
}

int Net::connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (ep->connected) {
        errno = EISCONN;
        return -1;
    }
    if (addr == nullptr || addrlen < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    if (addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    const uint16_t port = ntohs(reinterpret_cast<const struct sockaddr_in*>(addr)->sin_port);
    Endpoint* listener = find_listener(port);
    if (listener == nullptr || listener->pending.size() >= kMaxPending) {
        errno = ECONNREFUSED;
        return -1;
    }
    const int server_fd = create_endpoint();
    if (server_fd < 0) return -1;

    Endpoint& client = *ep;
    Endpoint& server = *get(server_fd);
    client.connected = true;
    client.port = next_port_++;
    client.peer = server_fd - kVirtualFdBase;
    server.connected = true;
    server.awaiting_accept = true;
    server.port = listener->port;
    server.peer = fd - kVirtualFdBase;
    listener->pending.push_back(server_fd - kVirtualFdBase);
    signal_change();
    return 0;
}

int Net::accept(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->listener) {
        errno = EINVAL;
        return -1;
    }
    while (ep->pending.empty()) {
        if (ep->nonblocking) {
            errno = EAGAIN;
            return -1;
        }
        scheduler_.mutex_lock(&mu_);
        while (ep->pending.empty())
            scheduler_.cond_wait(&cond_, &mu_);
        scheduler_.mutex_unlock(&mu_);
    }
    const int server_index = ep->pending.front();
    ep->pending.erase(ep->pending.begin());
    Endpoint& server = endpoints_[static_cast<size_t>(server_index)];
    server.awaiting_accept = false;
    const Endpoint* client =
        server.peer >= 0 ? &endpoints_[static_cast<size_t>(server.peer)] : nullptr;
    write_addr(addr, addrlen, client != nullptr ? client->port : 0);
    return kVirtualFdBase + server_index;
}

bool Net::readable(const Endpoint& ep) const {
    if (ep.listener) return !ep.pending.empty();
    if (!ep.rx.empty() && ep.rx.front().deliver_at <= clock_.now()) return true;
    return ep.peer_closed;
}

bool Net::room_for(const Endpoint& ep, size_t count) const {
    return ep.rx_bytes + count <= kSocketBufferBytes;
}

bool Net::writable(const Endpoint& ep) const {
    if (!ep.connected || ep.peer < 0) return false;
    return room_for(endpoints_[static_cast<size_t>(ep.peer)], 1);
}

void Net::signal_change() {
    // Deliberately unlocked: the scheduler is cooperative, so no waiter can be preempted between
    // its predicate check and its cond registration, and the broadcast cannot miss one.
    scheduler_.cond_broadcast(&cond_);
}

void Net::enqueue(Endpoint& peer, InjectedFault fault, const void* buf, size_t len, Time now) {
    Packet packet;
    packet.bytes.assign(static_cast<const uint8_t*>(buf), static_cast<const uint8_t*>(buf) + len);
    if (fault.kind == FaultKind::PacketCorrupt) {
        for (uint8_t& byte : packet.bytes)
            byte ^= 0x01;
    }
    packet.deliver_at = fault.kind == FaultKind::PacketDelay ? now + fault.amount : now;
    peer.rx_bytes += len;
    if (fault.kind == FaultKind::PacketReorder && !peer.rx.empty()) {
        peer.rx.insert(peer.rx.end() - 1, std::move(packet));
        return;
    }
    peer.rx.push_back(std::move(packet));
}

ssize_t Net::send(int fd, const void* buf, size_t len, int flags, InjectedFault fault) {
    (void)flags;
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected || ep->peer < 0) {
        errno = ENOTCONN;
        return -1;
    }
    // Real sockets also raise SIGPIPE here; a fiber cannot, and MSG_NOSIGNAL callers cannot tell.
    if (ep->peer_closed) {
        errno = EPIPE;
        return -1;
    }
    Endpoint& peer = endpoints_[static_cast<size_t>(ep->peer)];
    const size_t count = fault.kind == FaultKind::ShortSend ? (len >= 2 ? len / 2 : len) : len;
    if (count == 0) return 0;
    if (fault.kind == FaultKind::PacketDrop) return static_cast<ssize_t>(count);

    while (!room_for(peer, count)) {
        if (ep->nonblocking) {
            errno = EAGAIN;
            return -1;
        }
        scheduler_.mutex_lock(&mu_);
        while (!room_for(peer, count))
            scheduler_.cond_wait(&cond_, &mu_);
        scheduler_.mutex_unlock(&mu_);
    }

    enqueue(peer, fault, buf, count, clock_.now());
    signal_change();
    return static_cast<ssize_t>(count);
}

ssize_t Net::recv(int fd, void* buf, size_t len, int flags, InjectedFault fault) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected) {
        errno = ENOTCONN;
        return -1;
    }
    if (fault.kind == FaultKind::ConnReset) {
        errno = ECONNRESET;
        return -1;
    }
    // Marks the EOF and falls through: a real socket delivers what is already buffered first.
    if (fault.kind == FaultKind::PeerClose) ep->peer_closed = true;
    // A zero-length recv is not a read: the real call returns 0 without waiting for data.
    if (len == 0) return 0;
    const bool nonblocking = ep->nonblocking || (flags & MSG_DONTWAIT) != 0;
    while (true) {
        if (!ep->rx.empty()) {
            Packet& packet = ep->rx.front();
            if (packet.deliver_at > clock_.now()) {
                if (nonblocking) {
                    errno = EAGAIN;
                    return -1;
                }
                const int64_t wait_ns = (packet.deliver_at - clock_.now()).ns;
                struct timespec req{wait_ns / 1'000'000'000LL, wait_ns % 1'000'000'000LL};
                scheduler_.sleep_ns(&req, nullptr);
                continue;
            }
            const size_t available = packet.bytes.size() - packet.offset;
            const size_t count = available < len ? available : len;
            std::memcpy(buf, packet.bytes.data() + packet.offset, count);
            if ((flags & MSG_PEEK) == 0) {
                packet.offset += count;
                ep->rx_bytes -= count;
                if (packet.offset == packet.bytes.size()) ep->rx.erase(ep->rx.begin());
                // Any consumption frees room, and a send blocked on room must be woken by it.
                signal_change();
            }
            return static_cast<ssize_t>(count);
        }
        if (ep->peer_closed) return 0;
        if (nonblocking) {
            errno = EAGAIN;
            return -1;
        }
        scheduler_.mutex_lock(&mu_);
        while (ep->rx.empty() && !ep->peer_closed)
            scheduler_.cond_wait(&cond_, &mu_);
        scheduler_.mutex_unlock(&mu_);
    }
}

int Net::abort_pending(int fd) {
    Endpoint* ep = get(fd);
    if (ep == nullptr || !ep->listener || ep->pending.empty()) {
        errno = EINVAL;
        return -1;
    }
    const int index = ep->pending.front();
    ep->pending.erase(ep->pending.begin());
    release(endpoints_[static_cast<size_t>(index)]);
    signal_change();
    return 0;
}

int Net::shutdown(int fd, int how) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected || ep->peer < 0) {
        errno = ENOTCONN;
        return -1;
    }
    if (how == SHUT_RD || how == SHUT_RDWR) {
        ep->rx.clear();
        ep->rx_bytes = 0;
    }
    if (how == SHUT_WR || how == SHUT_RDWR) {
        endpoints_[static_cast<size_t>(ep->peer)].peer_closed = true;
    }
    signal_change();
    return 0;
}

void Net::release(Endpoint& ep) {
    if (ep.peer >= 0) {
        Endpoint& peer = endpoints_[static_cast<size_t>(ep.peer)];
        if (peer.used && peer.peer == static_cast<int>(&ep - endpoints_.data())) {
            peer.peer_closed = true;
        }
    }
    for (int pending : ep.pending) {
        Endpoint& server = endpoints_[static_cast<size_t>(pending)];
        if (server.used) release(server);
    }
    ep = Endpoint{};
}

int Net::close(int fd) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) return -1;
    release(*ep);
    signal_change();
    return 0;
}

int Net::poll(struct pollfd* fds, nfds_t count, int timeout_ms) {
    const Time start = clock_.now();
    while (true) {
        int ready = 0;
        for (nfds_t i = 0; i < count; ++i) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            const Endpoint* ep = get(fds[i].fd);
            if (ep == nullptr) {
                // A real descriptor under a universe is a regular file, which is always ready.
                fds[i].revents = static_cast<short>(fds[i].events & (POLLIN | POLLOUT));
                if (fds[i].revents != 0) ++ready;
                continue;
            }
            short revents = 0;
            if ((fds[i].events & POLLIN) != 0 && readable(*ep)) revents |= POLLIN;
            if ((fds[i].events & POLLOUT) != 0 && writable(*ep)) revents |= POLLOUT;
            if (ep->peer_closed) revents |= POLLHUP;
            fds[i].revents = revents;
            if (revents != 0) ++ready;
        }
        if (ready > 0 || count == 0 || timeout_ms == 0) return ready;

        if (timeout_ms < 0) {
            scheduler_.mutex_lock(&mu_);
            bool any = false;
            for (nfds_t i = 0; i < count && !any; ++i) {
                if (fds[i].fd < 0) continue;
                const Endpoint* ep = get(fds[i].fd);
                if (ep != nullptr && readable(*ep)) any = true;
            }
            if (!any) scheduler_.cond_wait(&cond_, &mu_);
            scheduler_.mutex_unlock(&mu_);
            continue;
        }

        const int64_t elapsed_ms = (clock_.now() - start).ns / 1'000'000LL;
        if (elapsed_ms >= timeout_ms) return 0;
        const int64_t remaining_ms = timeout_ms - elapsed_ms;
        struct timespec req{remaining_ms / 1000, (remaining_ms % 1000) * 1'000'000LL};
        scheduler_.sleep_ns(&req, nullptr);
    }
}

int Net::fcntl(int fd, int cmd, long arg) {
    Endpoint* ep = get(fd);
    if (ep == nullptr) return -1;
    switch (cmd) {
    case F_GETFL:
        return O_RDWR | (ep->nonblocking ? O_NONBLOCK : 0);
    case F_SETFL:
        ep->nonblocking = (arg & O_NONBLOCK) != 0;
        return 0;
    case F_GETFD:
        return 0;
    case F_SETFD:
        return 0;
    default:
        // Dup/lease/lock commands have no meaning for a virtual endpoint; P6 owns them.
        errno = EINVAL;
        return -1;
    }
}

int Net::peer_name(int fd, struct sockaddr* addr, socklen_t* addrlen) const {
    const Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    if (!ep->connected || ep->peer < 0) {
        errno = ENOTCONN;
        return -1;
    }
    write_addr(addr, addrlen, endpoints_[static_cast<size_t>(ep->peer)].port);
    return 0;
}

int Net::local_name(int fd, struct sockaddr* addr, socklen_t* addrlen) const {
    const Endpoint* ep = get(fd);
    if (ep == nullptr) {
        errno = EBADF;
        return -1;
    }
    write_addr(addr, addrlen, ep->port);
    return 0;
}

} // namespace cosmos

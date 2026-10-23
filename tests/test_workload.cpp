#include "cosmos/gen.hpp"
#include "cosmos/http.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/simulator.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <utility>

extern "C" ssize_t __wrap___read_chk(int fd, void* buf, size_t count, size_t buflen);

namespace {

// Fixed port is safe because each universe has its own virtual port table; it is never a host
// bind. The operation count is a fixed part of the deterministic transcript.
constexpr uint16_t kRestPort = 19100;
constexpr int kRestOperations = 40;

void must(bool ok) { assert(ok); }

// Some runners start with fd 0 closed. Several tests need a real descriptor there: set_stdin()
// captures its identity, and a closed fd 0 deliberately reports EBADF instead of harness bytes.
void ensure_open_stdin() {
    if (fcntl(STDIN_FILENO, F_GETFD) < 0) must(open("/dev/null", O_RDONLY) == STDIN_FILENO);
}

// ---- workload stream -------------------------------------------------------

void test_workload_rng_is_its_own_stream() {
    constexpr uint64_t seed = 4242;
    cosmos::Simulator sim(seed);
    cosmos::Rng expected(cosmos::stream_seed(seed, cosmos::StreamDomain::Workload));
    for (int i = 0; i < 128; ++i)
        must(sim.workload_rng().next() == expected.next());

    const uint64_t workload_seed = cosmos::stream_seed(seed, cosmos::StreamDomain::Workload);
    must(workload_seed != cosmos::stream_seed(seed, cosmos::StreamDomain::Schedule));
    must(workload_seed != cosmos::stream_seed(seed, cosmos::StreamDomain::Fault));
    must(workload_seed != cosmos::stream_seed(seed, cosmos::StreamDomain::User));
    must(workload_seed != cosmos::stream_seed(seed, cosmos::StreamDomain::Swarm));

    cosmos::Simulator a(seed);
    cosmos::Simulator b(seed);
    for (int i = 0; i < 1000; ++i)
        (void)a.workload_rng().next();
    must(a.user_rng().next() == b.user_rng().next());
    must(a.swarm_rng().next() == b.swarm_rng().next());

    cosmos::Simulator c(seed);
    cosmos::Simulator d(seed);
    for (int i = 0; i < 1000; ++i) {
        (void)c.user_rng().next();
        (void)c.swarm_rng().next();
    }
    must(c.workload_rng().next() == d.workload_rng().next());

    // Fault and schedule draws are separate streams too: consuming either must not move the
    // workload stream. A rule with a non-zero rate makes the fault draw reachable.
    cosmos::Simulator e(seed);
    cosmos::Simulator f(seed);
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = 0.5;
    must(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    must(plan.set_rule(cosmos::SiteId::malloc, std::move(rule)));
    must(e.install_faults(std::move(plan)).has_value());
    for (int i = 0; i < 1000; ++i) {
        (void)e.injector_or_null()->decide(cosmos::FaultClass::Memory, cosmos::SiteId::malloc);
        (void)e.scheduler().schedule_rng().next();
    }
    must(e.workload_rng().next() == f.workload_rng().next());
    std::cout << "[PASS] test_workload_rng_is_its_own_stream" << std::endl;
}

// ---- virtual stdin ---------------------------------------------------------

void test_stdin_serves_configured_bytes_then_eof() {
    ensure_open_stdin();
    cosmos::Simulator sim(1);
    cosmos::Simulator::set_current(&sim);
    sim.set_stdin("hello world");

    char first[8] = {};
    must(read(STDIN_FILENO, first, 5) == 5);
    must(std::memcmp(first, "hello", 5) == 0);
    must(sim.stdin_remaining() == 6);

    char second[8] = {};
    must(read(STDIN_FILENO, second, sizeof(second)) == 6);
    must(std::memcmp(second, " world", 6) == 0);
    must(sim.stdin_remaining() == 0);
    must(read(STDIN_FILENO, second, sizeof(second)) == 0);
    must(read(STDIN_FILENO, second, 0) == 0);

    // The fortified alias must route fd 0 to the same buffer.
    sim.set_stdin("alias");
    char alias[8] = {};
    must(__wrap___read_chk(STDIN_FILENO, alias, 5, sizeof(alias)) == 5);
    must(std::memcmp(alias, "alias", 5) == 0);
    must(__wrap___read_chk(STDIN_FILENO, alias, 1, sizeof(alias)) == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_stdin_serves_configured_bytes_then_eof" << std::endl;
}

void test_stdin_configuration_does_not_touch_the_tracked_heap() {
    cosmos::Simulator sim(2);
    cosmos::Simulator::set_current(&sim);
    const std::size_t before = sim.heap().active_count();

    // Positive control: a tracked malloc proves the counter is live in this build. (operator new
    // from a shared libstdc++ is not interposed, so a std::vector-based test could pass vacuously.)
    void* probe = malloc(16);
    must(probe != nullptr);
    must(sim.heap().active_count() == before + 1);
    free(probe);
    must(sim.heap().active_count() == before);

    sim.set_stdin("harness input");
    must(sim.heap().active_count() == before);
    sim.clear_stdin();
    must(sim.stdin_remaining() == 0);
    must(!sim.stdin_owned());
    must(sim.heap().active_count() == before);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_stdin_configuration_does_not_touch_the_tracked_heap" << std::endl;
}

void test_stdin_does_not_spend_storage_faults() {
    cosmos::Simulator sim(3);
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Storage);
    must(plan.activate_site(cosmos::SiteId::read));
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    must(rule.outcomes.add(cosmos::FaultKind::ReadEio, 1.0));
    must(plan.set_rule(cosmos::SiteId::read, std::move(rule)));
    must(sim.install_faults(std::move(plan)).has_value());

    cosmos::Simulator::set_current(&sim);
    const auto* injector = sim.injector_or_null();
    must(injector != nullptr);

    // Positive control: a real fd > 2 under the same rule spends one draw and gets EIO.
    ensure_open_stdin();
    char path[] = "/tmp/cosmos_stdin_fault_XXXXXX";
    const int file = mkstemp(path);
    must(file > STDERR_FILENO);
    must(write(file, "x", 1) == 1);
    must(lseek(file, 0, SEEK_SET) == 0);
    char file_byte = 0;
    errno = 0;
    must(read(file, &file_byte, 1) == -1);
    must(errno == EIO);
    must(injector->eligible_calls(cosmos::SiteId::read) == 1);
    must(injector->injections(cosmos::SiteId::read) == 1);
    must(close(file) == 0);
    must(unlink(path) == 0);

    // The stdin path must not move those counters at all.
    sim.set_stdin("fault-free");
    char buffer[16] = {};
    must(read(STDIN_FILENO, buffer, 10) == 10);
    must(std::memcmp(buffer, "fault-free", 10) == 0);
    must(injector->eligible_calls(cosmos::SiteId::read) == 1);
    must(injector->injections(cosmos::SiteId::read) == 1);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_stdin_does_not_spend_storage_faults" << std::endl;
}

void test_stdin_ownership_can_be_released() {
    cosmos::Simulator sim(4);
    cosmos::Simulator::set_current(&sim);
    sim.set_stdin("owned");
    must(sim.stdin_owned());
    sim.release_stdin();
    must(!sim.stdin_owned());
    sim.set_stdin("owned again");
    must(sim.stdin_owned());
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_stdin_ownership_can_be_released" << std::endl;
}

void test_close_and_open_release_stdin_ownership() {
    char path[] = "/tmp/cosmos_stdin_open_XXXXXX";
    const int seed = mkstemp(path);
    must(seed >= 0);
    must(write(seed, "Z", 1) == 1);
    must(close(seed) == 0);

    ensure_open_stdin();

    cosmos::Simulator sim(5);
    cosmos::Simulator::set_current(&sim);
    sim.set_stdin("harness");

    const int saved_stdin = dup(STDIN_FILENO); // host descriptor; dup is not interposed
    must(saved_stdin >= 0);
    must(close(STDIN_FILENO) == 0); // wrapper releases harness ownership
    must(!sim.stdin_owned());

    const int fd = open(path, O_RDONLY); // wrapper; the lowest free fd is 0
    must(fd == STDIN_FILENO);
    must(!sim.stdin_owned());

    char byte = 0;
    must(read(STDIN_FILENO, &byte, 1) == 1);
    must(byte == 'Z'); // the file, not the harness buffer
    must(close(fd) == 0);

    must(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO);
    must(close(saved_stdin) == 0);
    cosmos::Simulator::set_current(nullptr);
    must(unlink(path) == 0);
    std::cout << "[PASS] test_close_and_open_release_stdin_ownership" << std::endl;
}

// A tiny line-protocol app: "P <key> <value>\n" per record. The harness defines this shape and
// serializes generated operations into it; the app itself only sees read(0, ...).
struct StdinKvApp {
    std::map<std::string, std::string> store;
};

void process_stdin_line(StdinKvApp& app, std::string_view line) {
    if (!line.starts_with("P ")) return;
    const std::string_view rest = line.substr(2);
    const std::size_t space = rest.find(' ');
    if (space == std::string_view::npos) return;
    app.store[std::string(rest.substr(0, space))] = std::string(rest.substr(space + 1));
}

void run_stdin_app(StdinKvApp& app) {
    std::string line;
    char byte = 0;
    while (read(STDIN_FILENO, &byte, 1) == 1) {
        if (byte == '\n') {
            process_stdin_line(app, line);
            line.clear();
        } else {
            line.push_back(byte);
        }
    }
    process_stdin_line(app, line); // a final line without a newline is still a record
}

void test_dup2_replacement_is_detected_by_descriptor_identity() {
    char path[] = "/tmp/cosmos_stdin_dup_XXXXXX";
    const int file = mkstemp(path);
    must(file >= 0);
    must(write(file, "D", 1) == 1);
    must(lseek(file, 0, SEEK_SET) == 0);

    ensure_open_stdin();

    cosmos::Simulator sim(6);
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&sim);
    sim.set_stdin("harness");
    const int saved = dup(STDIN_FILENO);
    must(saved >= 0);

    // dup2 is not wrapped; the fstat identity check is what makes the replacement visible.
    must(dup2(file, STDIN_FILENO) == STDIN_FILENO);
    char byte = 0;
    must(read(STDIN_FILENO, &byte, 1) == 1);
    must(byte == 'D');
    must(sim.stdin_owned());
    must(!sim.stdin_descriptor_matches());

    must(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    must(close(saved) == 0);
    must(close(file) == 0);
    cosmos::Simulator::set_current(previous);
    must(unlink(path) == 0);
    std::cout << "[PASS] test_dup2_replacement_is_detected_by_descriptor_identity" << std::endl;
}

void test_close_ebadf_releases_stdin_ownership() {
    ensure_open_stdin();
    cosmos::Simulator sim(7);
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&sim);
    sim.set_stdin("owned");
    const int saved = dup(STDIN_FILENO);
    must(saved >= 0);

    must(close(STDIN_FILENO) == 0); // successful close releases
    sim.set_stdin("owned");         // arm again while fd 0 is closed
    must(sim.stdin_owned());
    errno = 0;
    must(close(STDIN_FILENO) == -1);
    must(errno == EBADF);
    must(!sim.stdin_owned()); // EBADF means no fd 0 remains: release as well

    must(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    must(close(saved) == 0);
    cosmos::Simulator::set_current(previous);
    std::cout << "[PASS] test_close_ebadf_releases_stdin_ownership" << std::endl;
}

void test_stdin_on_a_closed_descriptor_reports_ebadf() {
    ensure_open_stdin();
    cosmos::Simulator sim(8);
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&sim);
    const int saved = dup(STDIN_FILENO);
    must(saved >= 0);
    must(close(STDIN_FILENO) == 0);

    // Configuring a stream on a descriptor that does not exist must not conjure one: the contract
    // is the host's EBADF, not workload bytes handed to an application that probed for a closed
    // stdin. The identity capture fails here, so no descriptor can ever match it.
    sim.set_stdin("unreachable");
    must(sim.stdin_owned());
    must(!sim.stdin_descriptor_matches());
    char byte = 0;
    errno = 0;
    must(read(STDIN_FILENO, &byte, 1) == -1);
    must(errno == EBADF);

    must(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    must(close(saved) == 0);
    cosmos::Simulator::set_current(previous);
    std::cout << "[PASS] test_stdin_on_a_closed_descriptor_reports_ebadf" << std::endl;
}

void test_poll_answers_a_harness_stdin_without_reaching_the_host() {
    ensure_open_stdin();
    // fd 0 is an empty pipe read end: a host poll reports "not readable", and an infinite wait
    // there would park the universe's only OS thread. The harness buffer is regular-file-like and
    // always readable, so the transport must answer instead.
    int stdin_pipe[2] = {-1, -1};
    must(pipe(stdin_pipe) == 0);
    int spare_pipe[2] = {-1, -1};
    must(pipe(spare_pipe) == 0); // a different pipe, so its inode differs from stdin's
    must(stdin_pipe[0] > STDERR_FILENO && spare_pipe[0] > STDERR_FILENO);
    const int saved = dup(STDIN_FILENO);
    must(saved >= 0);
    must(dup2(stdin_pipe[0], STDIN_FILENO) == STDIN_FILENO);

    cosmos::Simulator sim(9);
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&sim);
    sim.set_stdin("polled");
    must(sim.stdin_descriptor_matches());

    // Timeout 0 keeps a regression a clean assertion failure rather than a hang; the routing
    // decision that avoids the hang is the same for every timeout.
    struct pollfd harness{STDIN_FILENO, POLLIN, 0};
    must(poll(&harness, 1, 0) == 1);
    must((harness.revents & POLLIN) != 0);

    // A replaced fd 0 belongs to the application, so readiness falls back to the host: the empty
    // spare pipe stays unreadable, which also proves the poll above did not consult it.
    must(dup2(spare_pipe[1], STDIN_FILENO) == STDIN_FILENO);
    must(!sim.stdin_descriptor_matches());
    struct pollfd replaced{STDIN_FILENO, POLLIN, 0};
    must(poll(&replaced, 1, 0) == 0);
    must(replaced.revents == 0);

    must(dup2(saved, STDIN_FILENO) == STDIN_FILENO);
    must(close(saved) == 0);
    must(close(stdin_pipe[0]) == 0);
    must(close(stdin_pipe[1]) == 0);
    must(close(spare_pipe[0]) == 0);
    must(close(spare_pipe[1]) == 0);
    cosmos::Simulator::set_current(previous);
    std::cout << "[PASS] test_poll_answers_a_harness_stdin_without_reaching_the_host" << std::endl;
}

void test_generated_stdin_workload_reaches_the_app() {
    ensure_open_stdin();
    cosmos::Simulator sim(555);
    std::string script;
    std::map<std::string, std::string> expected;
    cosmos::Rng& rng = sim.workload_rng();
    for (int i = 0; i < 32; ++i) {
        const std::string key = "k" + std::to_string(cosmos::gen::range<uint64_t>(rng, 0, 7));
        const std::string value = cosmos::gen::string(rng, 10);
        script += "P " + key + " " + value + "\n";
        expected[key] = value;
    }
    sim.set_stdin(script);

    StdinKvApp app;
    auto scenario = cosmos::Scenario::on(sim, cosmos::FaultConfig{});
    must(scenario.has_value());
    scenario->run([&] { run_stdin_app(app); });
    scenario->quiesce();
    scenario->check("stdin-workload-applied", [&] { return app.store == expected; });
    must(scenario->report().passed());
    std::cout << "[PASS] test_generated_stdin_workload_reaches_the_app" << std::endl;
}

// ---- a tiny in-process REST key/value server -------------------------------

struct RestKvServer {
    std::map<std::string, std::string> store;
    int listener = -1;
    bool stopping = false;
};

int listen_on(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    must(cosmos::is_virtual_fd(fd));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int connect_retry(uint16_t port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        must(cosmos::is_virtual_fd(fd));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        const struct timespec pause = {0, 1'000'000};
        nanosleep(&pause, nullptr);
    }
    return -1;
}

// Deliberately independent of cosmos::http::detail: the test server must not share the parser
// under test, or a bug in that parser could make both sides agree on the wrong frame.
bool send_all(int fd, std::string_view bytes) {
    // Chunk so a test response larger than the virtual socket buffer can still drain; Net::send
    // waits for room for the whole call, not a prefix.
    constexpr std::size_t kChunk = 4096;
    while (!bytes.empty()) {
        const std::size_t chunk = std::min(bytes.size(), kChunk);
        const ssize_t sent = send(fd, bytes.data(), chunk, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

std::string response_for(int status, std::string_view reason, std::string_view body) {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason;
    out += "\r\nContent-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n\r\n";
    out += body;
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

bool header_is(std::string_view name, std::string_view expected) {
    if (name.size() != expected.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(expected[i])));
        if (a != b) return false;
    }
    return true;
}

bool read_request(int fd, std::string& method, std::string& target, std::string& body) {
    std::string wire;
    std::size_t header_end = std::string::npos;
    while ((header_end = wire.find("\r\n\r\n")) == std::string::npos) {
        char buffer[1024];
        const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) return false;
        wire.append(buffer, static_cast<std::size_t>(got));
        if (wire.size() > 64 * 1024) return false;
    }

    const std::size_t line_end = wire.find("\r\n");
    const std::string_view request_line(wire.data(), line_end);
    const std::size_t first_space = request_line.find(' ');
    const std::size_t second_space = first_space == std::string_view::npos
                                         ? std::string_view::npos
                                         : request_line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
        return false;
    }
    method = std::string(request_line.substr(0, first_space));
    target = std::string(request_line.substr(first_space + 1, second_space - first_space - 1));

    std::size_t content_length = 0;
    std::size_t position = line_end + 2;
    while (position < header_end) {
        const std::size_t end = wire.find("\r\n", position);
        if (end == std::string::npos || end > header_end) return false;
        const std::string_view line(wire.data() + position, end - position);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) return false;
        const std::string_view name = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (header_is(name, "Content-Length")) {
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), content_length);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
        }
        position = end + 2;
    }

    const std::size_t body_begin = header_end + 4;
    while (wire.size() - body_begin < content_length) {
        char buffer[1024];
        const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) return false;
        wire.append(buffer, static_cast<std::size_t>(got));
    }
    body.assign(wire, body_begin, content_length);
    return true;
}

void serve_connection(RestKvServer& server, int fd) {
    std::string method;
    std::string target;
    std::string body;
    if (!read_request(fd, method, target, body)) return;

    std::string response;
    constexpr std::string_view prefix = "/kv/";
    if (target.rfind(prefix, 0) != 0) {
        response = response_for(404, "Not Found", "route");
    } else {
        const std::string key = target.substr(prefix.size());
        if (method == "PUT") {
            server.store[key] = body;
            response = response_for(200, "OK", "ok");
        } else if (method == "GET") {
            const auto found = server.store.find(key);
            response = found == server.store.end() ? response_for(404, "Not Found", "missing")
                                                   : response_for(200, "OK", found->second);
        } else {
            response = response_for(405, "Method Not Allowed", "method");
        }
    }
    (void)send_all(fd, response);
}

void* server_thread(void* raw) {
    auto* server = static_cast<RestKvServer*>(raw);
    const int listener = listen_on(kRestPort);
    server->listener = listener;
    if (listener < 0) return nullptr;
    while (!server->stopping) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) break;
        must(cosmos::is_virtual_fd(fd));
        serve_connection(*server, fd);
        close(fd);
    }
    server->listener = -1;
    return nullptr;
}

// ---- generated REST workload ----------------------------------------------

struct RestRun {
    std::string transcript;
    std::map<std::string, std::string> acknowledged;
    bool reads_matched = true;
    uint64_t puts = 0;
    uint64_t gets_200 = 0;
    uint64_t gets_404 = 0;
    uint64_t generated_puts = 0;
    uint64_t generated_gets = 0;
    uint64_t generated_hits = 0;
    uint64_t generated_misses = 0;
};

RestRun run_rest_workload(cosmos::Simulator& sim) {
    RestKvServer server;
    RestRun run;

    auto scenario = cosmos::Scenario::on(sim, cosmos::FaultConfig{});
    must(scenario.has_value());
    scenario->run([&] {
        pthread_t thread;
        must(pthread_create(&thread, nullptr, server_thread, &server) == 0);
        const struct timespec pause = {0, 1'000'000};
        nanosleep(&pause, nullptr); // let the server reach accept()

        auto put_key = [&](const std::string& key, const std::string& value) {
            const int fd = connect_retry(kRestPort);
            must(fd >= 0);
            const auto response = cosmos::http::put(fd, "/kv/" + key, value);
            close(fd);
            must(response.has_value());
            must(response->status == 200);
            run.acknowledged[key] = value;
            ++run.puts;
            run.transcript += "P:" + key + "=" + value + ";";
        };

        auto get_key = [&](const std::string& key) {
            const int fd = connect_retry(kRestPort);
            must(fd >= 0);
            const auto response = cosmos::http::get(fd, "/kv/" + key);
            close(fd);
            must(response.has_value());
            const auto expected = run.acknowledged.find(key);
            if (expected != run.acknowledged.end()) {
                run.reads_matched = run.reads_matched && response->status == 200 &&
                                    response->body == expected->second;
                if (response->status == 200) ++run.gets_200;
            } else {
                run.reads_matched = run.reads_matched && response->status == 404;
                if (response->status == 404) ++run.gets_404;
            }
            run.transcript +=
                "G:" + key + "=" + std::to_string(response->status) + ":" + response->body + ";";
            return response->status;
        };

        // Pin both read outcomes before the generated mix, so the oracle cannot pass by never
        // exercising a hit or a miss even if the generator's coin sequence changes.
        put_key("warm", "value");
        get_key("warm");
        get_key("absent");

        cosmos::Rng& rng = sim.workload_rng();
        for (int i = 0; i < kRestOperations; ++i) {
            const uint64_t key_number = cosmos::gen::range<uint64_t>(rng, 0, 7);
            const std::string key = "k" + std::to_string(key_number);
            if (cosmos::gen::coin(rng, 0.5)) {
                put_key(key, cosmos::gen::string(rng, 12));
                ++run.generated_puts;
            } else {
                const bool known = run.acknowledged.contains(key);
                const int status = get_key(key);
                ++run.generated_gets;
                if (known && status == 200) ++run.generated_hits;
                if (!known && status == 404) ++run.generated_misses;
            }
        }

        server.stopping = true;
        if (server.listener >= 0) {
            close(server.listener);
            server.listener = -1;
        }
        must(pthread_join(thread, nullptr) == 0);
    });

    scenario->quiesce();
    scenario->check("rest-read-your-writes", [&] {
        return run.reads_matched && run.puts > 0 && run.gets_200 > 0 && run.gets_404 > 0 &&
               run.generated_puts > 0 && run.generated_gets > 0 && run.generated_hits > 0 &&
               run.generated_misses > 0;
    });
    scenario->check("rest-store-has-every-acknowledged-write", [&] {
        if (server.store.size() != run.acknowledged.size()) return false; // no unacknowledged write
        for (const auto& [key, value] : run.acknowledged) {
            const auto found = server.store.find(key);
            if (found == server.store.end() || found->second != value) return false;
        }
        return true;
    });
    must(scenario->report().passed());
    return run;
}

void test_rest_put_larger_than_the_virtual_socket_buffer() {
    cosmos::Simulator sim(888);
    RestKvServer server;
    const std::string value(100 * 1024, 'x');

    auto scenario = cosmos::Scenario::on(sim, cosmos::FaultConfig{});
    must(scenario.has_value());
    scenario->run([&] {
        pthread_t thread;
        must(pthread_create(&thread, nullptr, server_thread, &server) == 0);
        const struct timespec pause = {0, 1'000'000};
        nanosleep(&pause, nullptr);

        int fd = connect_retry(kRestPort);
        must(fd >= 0);
        const auto put_response = cosmos::http::put(fd, "/kv/big", value);
        close(fd);
        must(put_response.has_value());
        must(put_response->status == 200);

        fd = connect_retry(kRestPort);
        must(fd >= 0);
        const auto get_response = cosmos::http::get(fd, "/kv/big");
        close(fd);
        must(get_response.has_value());
        must(get_response->status == 200);
        must(get_response->body == value);

        server.stopping = true;
        if (server.listener >= 0) {
            close(server.listener);
            server.listener = -1;
        }
        must(pthread_join(thread, nullptr) == 0);
    });
    scenario->quiesce();
    scenario->check("large-put-round-trip", [&] {
        const auto found = server.store.find("big");
        return found != server.store.end() && found->second == value;
    });
    must(scenario->report().passed());
    std::cout << "[PASS] test_rest_put_larger_than_the_virtual_socket_buffer" << std::endl;
}

// ---- HTTP framing and request-validation tests ----------------------------

struct ScriptedPeer {
    int fd = -1;
    std::string response;
};

void* scripted_peer_thread(void* raw) {
    auto* peer = static_cast<ScriptedPeer*>(raw);
    std::string request;
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t got = recv(peer->fd, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<std::size_t>(got));
    }
    (void)send_all(peer->fd, peer->response);
    close(peer->fd);
    return nullptr;
}

std::expected<cosmos::http::Response, cosmos::http::Error>
exchange_with_script(std::string response, cosmos::http::Request request) {
    cosmos::Simulator sim(6060);
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&sim);
    int sv[2] = {-1, -1};
    must(socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0);
    must(cosmos::is_virtual_fd(sv[0]) && cosmos::is_virtual_fd(sv[1]));

    ScriptedPeer peer{sv[1], std::move(response)};
    pthread_t thread;
    must(pthread_create(&thread, nullptr, scripted_peer_thread, &peer) == 0);
    auto result = cosmos::http::exchange(sv[0], request);
    close(sv[0]);
    must(pthread_join(thread, nullptr) == 0);
    cosmos::Simulator::set_current(previous);
    return result;
}

void test_http_client_rejects_bad_framing_and_accepts_valid_frames() {
    using cosmos::http::Error;
    using cosmos::http::Request;

    auto chunked = exchange_with_script(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", Request{});
    must(!chunked && chunked.error() == Error::UnsupportedTransferEncoding);

    auto duplicate_length = exchange_with_script(
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx", Request{});
    must(!duplicate_length && duplicate_length.error() == Error::MalformedHeaders);

    auto bad_header = exchange_with_script("HTTP/1.1 200 OK\r\nNoColonHere\r\n\r\n", Request{});
    must(!bad_header && bad_header.error() == Error::MalformedHeaders);

    auto bad_status = exchange_with_script("BOGUS\r\n\r\n", Request{});
    must(!bad_status && bad_status.error() == Error::MalformedStatus);

    auto bad_status_range = exchange_with_script("HTTP/1.1 999 Nope\r\n\r\n", Request{});
    must(!bad_status_range && bad_status_range.error() == Error::MalformedStatus);

    auto truncated =
        exchange_with_script("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc", Request{});
    must(!truncated && truncated.error() == Error::Truncated);

    auto eof_framed =
        exchange_with_script("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello", Request{});
    must(eof_framed.has_value());
    must(eof_framed->status == 200 && eof_framed->body == "hello");

    // 100 Continue is informational; the final response follows in the same byte stream.
    auto continued = exchange_with_script(
        "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", Request{});
    must(continued.has_value());
    must(continued->status == 200 && continued->body == "ok");

    // HEAD and 204/304 carry a Content-Length but no body; the client must not wait for one.
    Request head;
    head.method = "HEAD";
    auto head_response = exchange_with_script("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", head);
    must(head_response.has_value());
    must(head_response->status == 200 && head_response->body.empty());

    auto no_content =
        exchange_with_script("HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n", Request{});
    must(no_content.has_value());
    must(no_content->status == 204 && no_content->body.empty());

    auto not_modified =
        exchange_with_script("HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\n", Request{});
    must(not_modified.has_value());
    must(not_modified->status == 304 && not_modified->body.empty());

    auto upgrade = exchange_with_script("HTTP/1.1 101 Switching Protocols\r\n\r\n", Request{});
    must(!upgrade && upgrade.error() == Error::UnsupportedResponse);

    // A HEAD/204/304 Content-Length describes bytes that are not sent; a large value must not be
    // mistaken for a body that needs the body cap.
    auto head_large =
        exchange_with_script("HTTP/1.1 200 OK\r\nContent-Length: 104857600\r\n\r\n", head);
    must(head_large.has_value());
    must(head_large->status == 200 && head_large->body.empty());

    auto no_content_large = exchange_with_script(
        "HTTP/1.1 204 No Content\r\nContent-Length: 104857600\r\n\r\n", Request{});
    must(no_content_large.has_value());
    must(no_content_large->status == 204 && no_content_large->body.empty());

    auto duplicate_te = exchange_with_script(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\nTransfer-Encoding: identity\r\n\r\n",
        Request{});
    must(!duplicate_te && duplicate_te.error() == Error::MalformedHeaders);

    auto identity_with_length = exchange_with_script(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\nContent-Length: 2\r\n\r\nok", Request{});
    must(!identity_with_length && identity_with_length.error() == Error::MalformedHeaders);

    // HEAD/304 may advertise the coding a GET would use; there is no body to decode.
    auto head_chunked = exchange_with_script(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n", head);
    must(head_chunked.has_value());
    must(head_chunked->status == 200 && head_chunked->body.empty());

    auto not_modified_chunked = exchange_with_script(
        "HTTP/1.1 304 Not Modified\r\nTransfer-Encoding: chunked\r\n\r\n", Request{});
    must(not_modified_chunked.has_value());
    must(not_modified_chunked->status == 304 && not_modified_chunked->body.empty());

    auto identity_eof = exchange_with_script(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\nabc", Request{});
    must(identity_eof.has_value());
    must(identity_eof->body == "abc");

    auto reset_content =
        exchange_with_script("HTTP/1.1 205 Reset Content\r\nContent-Length: 5\r\n\r\n", Request{});
    must(reset_content.has_value());
    must(reset_content->status == 205 && reset_content->body.empty());

    Request connect_request;
    connect_request.method = "CONNECT";
    connect_request.target = "example.com:443";
    auto connect_response =
        exchange_with_script("HTTP/1.1 200 Connection Established\r\n\r\n", connect_request);
    must(!connect_response && connect_response.error() == Error::UnsupportedResponse);

    auto empty_response = exchange_with_script("", Request{});
    must(!empty_response && empty_response.error() == Error::Truncated);

    auto bare_eof = exchange_with_script("HTTP/1.1 200 OK\r\n\r\n", Request{});
    must(bare_eof.has_value());
    must(bare_eof->status == 200 && bare_eof->body.empty());

    std::string big_head = "HTTP/1.1 200 OK\r\nX-Big: ";
    big_head.append(70 * 1024, 'a');
    big_head += "\r\n\r\n";
    auto too_large_head = exchange_with_script(big_head, Request{});
    must(!too_large_head && too_large_head.error() == Error::TooLarge);

    auto too_large_body =
        exchange_with_script("HTTP/1.1 200 OK\r\nContent-Length: 67108865\r\n\r\n", Request{});
    must(!too_large_body && too_large_body.error() == Error::TooLarge);

    std::string header_overflow = "HTTP/1.1 200 OK\r\n";
    for (int i = 0; i < 300; ++i) {
        header_overflow += "X-Overflow-" + std::to_string(i) + ": v\r\n";
    }
    header_overflow += "Content-Length: 0\r\n\r\n";
    auto too_many_headers = exchange_with_script(header_overflow, Request{});
    must(!too_many_headers && too_many_headers.error() == Error::TooLarge);

    auto leading_zero_length =
        exchange_with_script("HTTP/1.1 200 OK\r\nContent-Length: 007\r\n\r\n", Request{});
    must(!leading_zero_length && leading_zero_length.error() == Error::MalformedHeaders);

    auto server_keep_alive =
        exchange_with_script("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n\r\nhello", Request{});
    must(!server_keep_alive && server_keep_alive.error() == Error::UnsupportedResponse);

    auto bad_reason = exchange_with_script(
        "HTTP/1.1 200 OK\nInjected: yes\r\nContent-Length: 0\r\n\r\n", Request{});
    must(!bad_reason && bad_reason.error() == Error::MalformedStatus);

    std::string too_many_informational;
    for (int i = 0; i < 6; ++i)
        too_many_informational += "HTTP/1.1 100 Continue\r\n\r\n";
    auto informational_chain = exchange_with_script(too_many_informational, Request{});
    must(!informational_chain && informational_chain.error() == Error::TooLarge);

    // Request validation: CRLF in the target or a mismatched caller Content-Length is refused.
    Request bad_target;
    bad_target.target = "/a\r\nX: y";
    auto rejected_target = exchange_with_script("", bad_target);
    must(!rejected_target && rejected_target.error() == Error::MalformedRequest);

    Request bad_length;
    bad_length.headers.push_back({"Content-Length", "9"});
    auto rejected_length = exchange_with_script("", bad_length);
    must(!rejected_length && rejected_length.error() == Error::MalformedRequest);

    Request duplicate_request_length;
    duplicate_request_length.headers.push_back({"Content-Length", "0"});
    duplicate_request_length.headers.push_back({"Content-Length", "0"});
    auto rejected_duplicate_length = exchange_with_script("", duplicate_request_length);
    must(!rejected_duplicate_length &&
         rejected_duplicate_length.error() == Error::MalformedRequest);

    Request te_request;
    te_request.headers.push_back({"Transfer-Encoding", "chunked"});
    auto rejected_te = exchange_with_script("", te_request);
    must(!rejected_te && rejected_te.error() == Error::UnsupportedTransferEncoding);

    Request injected_header;
    injected_header.headers.push_back({"X-Test", "a\r\nInjected: yes"});
    auto rejected_injection = exchange_with_script("", injected_header);
    must(!rejected_injection && rejected_injection.error() == Error::MalformedRequest);

    Request injected_name;
    injected_name.headers.push_back({"X-Test: x", "v"});
    auto rejected_name = exchange_with_script("", injected_name);
    must(!rejected_name && rejected_name.error() == Error::MalformedRequest);

    Request duplicate_host;
    duplicate_host.headers.push_back({"Host", "a"});
    duplicate_host.headers.push_back({"Host", "b"});
    auto rejected_host = exchange_with_script("", duplicate_host);
    must(!rejected_host && rejected_host.error() == Error::MalformedRequest);

    Request keep_alive;
    keep_alive.headers.push_back({"Connection", "keep-alive"});
    auto rejected_keep_alive = exchange_with_script("", keep_alive);
    must(!rejected_keep_alive && rejected_keep_alive.error() == Error::MalformedRequest);

    // Public header lookup is case-insensitive.
    auto found = exchange_with_script(
        "HTTP/1.1 201 Created\r\nX-Test: yes\r\nContent-Length: 0\r\n\r\n", Request{});
    must(found.has_value());
    const auto* header = cosmos::http::find_header(*found, "x-test");
    must(header != nullptr && header->value == "yes");
    std::cout << "[PASS] test_http_client_rejects_bad_framing_and_accepts_valid_frames"
              << std::endl;
}

void test_http_client_reports_send_failure() {
    cosmos::Simulator sim(6061);
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&sim);
    int sv[2] = {-1, -1};
    must(socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0);
    must(close(sv[1]) == 0); // peer gone: the request send is a broken pipe
    const auto result = cosmos::http::exchange(sv[0], cosmos::http::Request{});
    must(close(sv[0]) == 0);
    cosmos::Simulator::set_current(previous);
    must(!result && result.error() == cosmos::http::Error::SendFailed);
    std::cout << "[PASS] test_http_client_reports_send_failure" << std::endl;
}

void* corrupt_peer_thread(void* raw) {
    auto* peer = static_cast<ScriptedPeer*>(raw);
    char buffer[1024];
    (void)recv(peer->fd, buffer, sizeof(buffer), 0); // consume the (also corrupted) request
    (void)send_all(peer->fd, peer->response);
    close(peer->fd);
    return nullptr;
}

void test_http_client_survives_a_corrupted_network_stream() {
    cosmos::Simulator sim(6062);
    auto* previous = cosmos::Simulator::current();
    cosmos::Simulator::set_current(&sim);

    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Network);
    must(plan.activate_site(cosmos::SiteId::send));
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    must(rule.outcomes.add(cosmos::FaultKind::PacketCorrupt, 1.0));
    must(plan.set_rule(cosmos::SiteId::send, std::move(rule)));
    must(sim.install_faults(std::move(plan)).has_value());

    int sv[2] = {-1, -1};
    must(socketpair(AF_INET, SOCK_STREAM, 0, sv) == 0);
    ScriptedPeer peer{sv[1], "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"};
    pthread_t thread;
    must(pthread_create(&thread, nullptr, corrupt_peer_thread, &peer) == 0);
    const auto result = cosmos::http::exchange(sv[0], cosmos::http::Request{});
    must(close(sv[0]) == 0);
    must(pthread_join(thread, nullptr) == 0);
    cosmos::Simulator::set_current(previous);
    // PacketCorrupt flips every byte, including the CRLFs, so the header terminator never
    // appears; the server's close turns that into Truncated.
    must(!result && result.error() == cosmos::http::Error::Truncated);
    std::cout << "[PASS] test_http_client_survives_a_corrupted_network_stream" << std::endl;
}

void test_generated_rest_workload_passes_and_is_deterministic() {
    cosmos::Simulator first(777);
    const RestRun a = run_rest_workload(first);
    cosmos::Simulator second(777);
    const RestRun b = run_rest_workload(second);
    must(a.transcript == b.transcript);
    must(a.acknowledged == b.acknowledged);
    must(a.reads_matched && b.reads_matched);
    std::cout << "[PASS] test_generated_rest_workload_passes_and_is_deterministic" << std::endl;
}

} // namespace

int main() {
    test_workload_rng_is_its_own_stream();
    test_stdin_serves_configured_bytes_then_eof();
    test_stdin_configuration_does_not_touch_the_tracked_heap();
    test_stdin_does_not_spend_storage_faults();
    test_stdin_ownership_can_be_released();
    test_close_and_open_release_stdin_ownership();
    test_dup2_replacement_is_detected_by_descriptor_identity();
    test_close_ebadf_releases_stdin_ownership();
    test_stdin_on_a_closed_descriptor_reports_ebadf();
    test_poll_answers_a_harness_stdin_without_reaching_the_host();
    test_generated_stdin_workload_reaches_the_app();
    test_http_client_rejects_bad_framing_and_accepts_valid_frames();
    test_http_client_reports_send_failure();
    test_http_client_survives_a_corrupted_network_stream();
    test_rest_put_larger_than_the_virtual_socket_buffer();
    test_generated_rest_workload_passes_and_is_deterministic();
    std::cout << "All workload tests passed." << std::endl;
    return 0;
}

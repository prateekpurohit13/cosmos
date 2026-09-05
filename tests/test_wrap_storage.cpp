#include "cosmos/cosmos.hpp"

// White-box include: this test pins the internal storage eligibility policy, not just the
// public behavior. See the target_include_directories entry in tests/CMakeLists.txt.
#include "wrapper_fault.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Every kind a storage wrapper can emit must stay legal for its site (Rule 15); a taxonomy
// change that breaks one of these is a compile error here, not a false positive in a campaign.
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::open, cosmos::FaultKind::OpenEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::open, cosmos::FaultKind::NoSpace));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::read, cosmos::FaultKind::ReadEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::write, cosmos::FaultKind::WriteEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::write, cosmos::FaultKind::ShortWrite));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::write, cosmos::FaultKind::NoSpace));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::fsync, cosmos::FaultKind::FsyncEio));
static_assert(cosmos::is_legal_outcome(cosmos::SiteId::fsync, cosmos::FaultKind::NoSpace));
// Nothing else is: spot-check the cross-site borrowings validate() must reject.
static_assert(!cosmos::is_legal_outcome(cosmos::SiteId::read, cosmos::FaultKind::NoSpace));
static_assert(!cosmos::is_legal_outcome(cosmos::SiteId::open, cosmos::FaultKind::ReadEio));

// Eligibility policy, pinned at compile time (white-box). Without an injector the wrapper's
// decision path folds to None, so only these static asserts can prove the policy itself;
// end-to-end eligibility through a live injector lands with F2.
static_assert(!cosmos::wrappers::storage_fd_eligible(0));
static_assert(!cosmos::wrappers::storage_fd_eligible(1));
static_assert(!cosmos::wrappers::storage_fd_eligible(2));
static_assert(!cosmos::wrappers::storage_fd_eligible(-1));
static_assert(cosmos::wrappers::storage_fd_eligible(3));
static_assert(!cosmos::wrappers::storage_write_eligible(3, 0)); // empty transfer
static_assert(cosmos::wrappers::storage_write_eligible(3, 1));  // 1-byte writes stay eligible
static_assert(cosmos::wrappers::storage_write_eligible(3, 4096));
static_assert(!cosmos::wrappers::storage_write_eligible(1, 4096)); // std stream fd
static_assert(!cosmos::wrappers::storage_read_eligible(3, 0));
static_assert(cosmos::wrappers::storage_read_eligible(3, 128));
static_assert(!cosmos::wrappers::storage_read_eligible(2, 128)); // std stream fd

namespace {

std::string g_temp_dir;

std::string temp_path(const char* tag) {
    static int counter = 0;
    return g_temp_dir + "/cosmos_storage_" + tag + "_" + std::to_string(getpid()) + "_" +
           std::to_string(counter++);
}

} // namespace

void test_passthrough_no_sim() {
    assert(!cosmos::Simulator::has_current());

    const std::string path = temp_path("passthrough");
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    const char msg[] = "hello cosmos";
    assert(write(fd, msg, sizeof(msg)) == (ssize_t)sizeof(msg));
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);

    fd = open(path.c_str(), O_RDONLY);
    assert(fd >= 0);
    char buf[64] = {};
    assert(read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(msg));
    assert(strcmp(buf, msg) == 0);
    assert(close(fd) == 0);
    unlink(path.c_str());

    std::cout << "[PASS] test_passthrough_no_sim" << std::endl;
}

// The NoInjector folding hook must leave the happy path untouched: a full file round-trip
// inside an active Simulator behaves exactly like the host, and successes never clobber errno.
void test_value_path_under_sim() {
    const std::string path = temp_path("value");

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    const char msg[] = "deterministic bytes";
    const size_t len = sizeof(msg) - 1;
    assert(write(fd, msg, len) == (ssize_t)len);
    assert(fsync(fd) == 0);
    assert(close(fd) == 0);

    fd = open(path.c_str(), O_RDONLY);
    assert(fd >= 0);
    char buf[64] = {};
    assert(read(fd, buf, sizeof(buf)) == (ssize_t)len);
    assert(strcmp(buf, msg) == 0);
    assert(close(fd) == 0);
    assert(errno == 0);
    unlink(path.c_str());

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_value_path_under_sim" << std::endl;
}

// Zero-length I/O is never eligible: the real call answers, including on odd fds.
// 1-byte writes are eligible (they can legally fail at F2) and pass through today.
void test_zero_length_io_passthrough() {
    const std::string path = temp_path("tiny");

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    const char buf[] = {'a', 'b', 'c', 'd'};
    assert(write(fd, buf, 0) == 0); // zero-length: passthrough, no decision
    assert(write(fd, buf, 1) == 1); // 1-byte: eligible, honest full write today

    char in[8] = {};
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(read(fd, in, sizeof(in)) == 1); // exactly the one written byte landed
    assert(in[0] == 'a');
    assert(read(fd, in, 0) == 0); // zero-length read: passthrough
    assert(close(fd) == 0);
    assert(errno == 0);
    unlink(path.c_str());

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_zero_length_io_passthrough" << std::endl;
}

// Standard stream fds are never eligible: logging must stay draw-free and unfaulted once a
// real injector is wired (F2). Today this proves the passthrough is intact on fds 1 and 2.
void test_std_stream_fds_pass_through() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    const char probe[] = "(cosmos: std-stream passthrough probe)\n";
    assert(write(1, probe, sizeof(probe) - 1) == (ssize_t)(sizeof(probe) - 1));
    assert(write(2, probe, sizeof(probe) - 1) == (ssize_t)(sizeof(probe) - 1));
    assert(errno == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_std_stream_fds_pass_through" << std::endl;
}

// The variadic mode argument must survive the wrapper on both paths: O_CREAT files carry the
// requested permissions (masked by the process umask, exactly like the real call).
void test_open_mode_forwarding() {
    const mode_t mask = umask(0);
    umask(mask);
    const mode_t expect = static_cast<mode_t>(0640 & ~mask);

    const std::string host_path = temp_path("mode_host");
    int fd = open(host_path.c_str(), O_CREAT | O_WRONLY, 0640);
    assert(fd >= 0);
    struct stat st{};
    assert(fstat(fd, &st) == 0);
    assert((st.st_mode & 0777) == expect);
    assert(close(fd) == 0);
    unlink(host_path.c_str());

    const std::string sim_path = temp_path("mode_sim");
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);
    fd = open(sim_path.c_str(), O_CREAT | O_WRONLY, 0640);
    assert(fd >= 0);
    assert(fstat(fd, &st) == 0);
    assert((st.st_mode & 0777) == expect);
    assert(close(fd) == 0);
    unlink(sim_path.c_str());
    cosmos::Simulator::set_current(nullptr);

    std::cout << "[PASS] test_open_mode_forwarding" << std::endl;
}

// The wrapper path must not allocate (Rule 7): with malloc wrapped, any internal allocation
// while the sim is current would land on the tracked heap and trip the counters.
void test_no_alloc_smoke() {
    const std::string path = temp_path("alloc");

    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);
    char buf[16] = {};
    for (int i = 0; i < 10000; ++i) {
        assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
        assert(lseek(fd, 0, SEEK_SET) == 0);
        assert(read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
    }
    for (int i = 0; i < 100; ++i) {
        assert(fsync(fd) == 0);
    }
    assert(close(fd) == 0);
    unlink(path.c_str());

    assert(sim.heap().stats().active_allocations == 0);
    assert(sim.heap().stats().total_allocation_count == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_no_alloc_smoke" << std::endl;
}

int main() {
    // Unique 0700 directory: no dependence on a pre-existing path, no cross-user collisions.
    std::string tmpl = "/tmp/cosmos_storage_XXXXXX";
    if (mkdtemp(tmpl.data()) == nullptr) {
        std::cerr << "mkdtemp failed: " << strerror(errno) << std::endl;
        return 1;
    }
    g_temp_dir = tmpl;

    test_passthrough_no_sim();
    test_value_path_under_sim();
    test_zero_length_io_passthrough();
    test_std_stream_fds_pass_through();
    test_open_mode_forwarding();
    test_no_alloc_smoke();

    rmdir(g_temp_dir.c_str()); // best effort; every test unlinks its own files
    std::cout << "All storage wrapper tests passed successfully!" << std::endl;
    return 0;
}

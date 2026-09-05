#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/random.h>
#include <sys/types.h>

namespace {

unsigned allowed_getrandom_flags() {
    unsigned allowed = 0;
#ifdef GRND_NONBLOCK
    allowed |= static_cast<unsigned>(GRND_NONBLOCK);
#endif
#ifdef GRND_RANDOM
    allowed |= static_cast<unsigned>(GRND_RANDOM);
#endif
#ifdef GRND_INSECURE
    allowed |= static_cast<unsigned>(GRND_INSECURE);
#endif
    return allowed;
}

} // namespace

extern "C" {

ssize_t __real_getrandom(void* buf, size_t buflen, unsigned int flags);
long int __real_random(void);
int __real_rand(void);

ssize_t __wrap_getrandom(void* buf, size_t buflen, unsigned int flags) {
    // Passthrough first, like every other wrapper: outside a universe the host kernel owns all
    // validation, including flag checks and zero-length semantics.
    if (!cosmos::Simulator::has_current()) {
        return __real_getrandom(buf, buflen, flags);
    }

    // Inside a universe, mirror the kernel's validation order (drivers/char/random.c,
    // SYSCALL_DEFINE3(getrandom)): unknown flag bits, then the INSECURE|RANDOM combination,
    // then count clamping, then the count == 0 short-circuit, then the buffer itself. Calling
    // getrandom(nullptr, 16, 0x80000000) must fail with EINVAL, not EFAULT, and
    // getrandom(buf, 0, 0x80000000) must fail with EINVAL, not succeed with 0.
    if ((flags & ~allowed_getrandom_flags()) != 0) {
        errno = EINVAL;
        return -1;
    }
#if defined(GRND_INSECURE) && defined(GRND_RANDOM)
    if ((flags & static_cast<unsigned>(GRND_INSECURE)) != 0 &&
        (flags & static_cast<unsigned>(GRND_RANDOM)) != 0) {
        errno = EINVAL;
        return -1;
    }
#endif

    // The kernel clamps count to INT_MAX; mirroring that keeps the ssize_t cast below from
    // overflowing negative and misrepresenting success as an unknown error.
    const size_t count =
        buflen > static_cast<size_t>(INT_MAX) ? static_cast<size_t>(INT_MAX) : buflen;
    if (count == 0) {
        return 0;
    }
    if (buf == nullptr) {
        errno = EFAULT;
        return -1;
    }

    auto* sim = cosmos::Simulator::current();
    // Decision first, values second: a failed call must not consume the User stream (Rule 1).
    if (cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Random, cosmos::SiteId::getrandom) ==
        cosmos::FaultKind::RandomEagain) {
        errno = EAGAIN;
        return -1;
    }

    // Deterministic bytes from the User stream, 8 at a time. Allocation-free (Rule 7): stack
    // word plus memcpy, no malloc, no errno clobber on success.
    auto* out = static_cast<unsigned char*>(buf);
    size_t remaining = count;
    while (remaining > 0) {
        const uint64_t word = sim->user_rng().next();
        const size_t chunk = remaining < sizeof(word) ? remaining : sizeof(word);
        memcpy(out, &word, chunk);
        out += chunk;
        remaining -= chunk;
    }
    return static_cast<ssize_t>(count);
}

long int __wrap_random(void) {
    if (!cosmos::Simulator::has_current()) {
        return __real_random();
    }
    // No SiteId::random and no legal FaultKind: value-only, never draws from a fault stream.
    auto* sim = cosmos::Simulator::current();
    return static_cast<long int>(sim->user_rng().range(0, static_cast<uint64_t>(RAND_MAX)));
}

// glibc's rand() is a separate public symbol whose internal path does not re-enter our wrapped
// random(), so it must be wrapped on its own or it leaks host nondeterminism into a run. It
// shares the User stream with random(): both are user-visible draws, consumed in call order.
int __wrap_rand(void) {
    if (!cosmos::Simulator::has_current()) {
        return __real_rand();
    }
    auto* sim = cosmos::Simulator::current();
    return static_cast<int>(sim->user_rng().range(0, static_cast<uint64_t>(RAND_MAX)));
}

// Deterministic no-ops: host seeding must not perturb the User stream. Deliberately do not touch
// any RNG, fault stream, or errno.
void __wrap_srandom(unsigned int seed) { (void)seed; }

void __wrap_srand(unsigned int seed) { (void)seed; }

} // extern "C"

// POSIX time wrappers route to the current universe's VirtualClock; without an active
// Simulator on this thread they fall through to the real host clock. Sleeps go
// through the fiber scheduler so one task's sleep suspends only itself and
// virtual time advances only when no fiber is runnable (docs/design.md §10).
#include "cosmos/cosmos.hpp"
#include <sys/time.h>
#include <time.h>

extern "C" {

int __real_clock_gettime(clockid_t clock_id, struct timespec* tp);
int __real_gettimeofday(struct timeval* tv, void* tz);
int __real_nanosleep(const struct timespec* req, struct timespec* rem);
int __real_clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* req,
                           struct timespec* rem);

int __wrap_clock_gettime(clockid_t clock_id, struct timespec* tp) {
    if (!cosmos::Simulator::has_current()) {
        return __real_clock_gettime(clock_id, tp);
    }
    return cosmos::Simulator::current()->clock().clock_gettime(clock_id, tp);
}

int __wrap_gettimeofday(struct timeval* tv, void* tz) {
    if (!cosmos::Simulator::has_current()) {
        return __real_gettimeofday(tv, tz);
    }
    return cosmos::Simulator::current()->clock().gettimeofday(tv, tz);
}

int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!cosmos::Simulator::has_current()) {
        return __real_nanosleep(req, rem);
    }
    return cosmos::Simulator::current()->scheduler().sleep_ns(req, rem);
}

int __wrap_clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* req,
                           struct timespec* rem) {
    if (!cosmos::Simulator::has_current()) {
        return __real_clock_nanosleep(clock_id, flags, req, rem);
    }
    return cosmos::Simulator::current()->scheduler().sleep_clock(clock_id, flags, req, rem);
}

} // extern "C"

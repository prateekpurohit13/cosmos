// POSIX time wrappers route to the current universe's VirtualClock; without an active
// Simulator on this thread they fall through to the real host clock.
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
    return cosmos::Simulator::current()->clock().nanosleep(req, rem);
}

int __wrap_clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* req,
                           struct timespec* rem) {
    if (!cosmos::Simulator::has_current()) {
        return __real_clock_nanosleep(clock_id, flags, req, rem);
    }
    return cosmos::Simulator::current()->clock().clock_nanosleep(clock_id, flags, req, rem);
}

} // extern "C"

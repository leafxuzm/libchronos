#pragma once

#include <cstdint>

#if defined(__linux__)
#include <pthread.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <sys/sysctl.h>
#endif

namespace chronos {
namespace utils {

/// Pin the calling thread to a specific CPU core.
///
/// Linux:   pthread_setaffinity_np  — full kernel-level pinning.
/// macOS:   thread_policy_set(THREAD_AFFINITY_POLICY) — Mach-level hint.
///
/// @param cpu  zero-based core index, or -1 to clear any affinity.
/// @return true if the platform supports affinity and the call succeeded.
inline bool setCpuAffinity(int cpu) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (cpu >= 0) {
        CPU_SET(static_cast<unsigned>(cpu), &cpuset);
    } else {
        // -1: restore to all cores
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        for (int i = 0; i < (nprocs > 0 ? nprocs : CPU_SETSIZE); ++i) {
            CPU_SET(static_cast<unsigned>(i), &cpuset);
        }
    }
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;

#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy;
    if (cpu >= 0) {
        policy.affinity_tag = static_cast<integer_t>(cpu);
    } else {
        policy.affinity_tag = 0;  // default scheduling
    }
    return thread_policy_set(mach_thread_self(),
                             THREAD_AFFINITY_POLICY,
                             (thread_policy_t)&policy,
                             THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;

#else
    (void)cpu;
    return false;
#endif
}

/// Return the number of available CPU cores (best-effort).
inline int numCores() {
#if defined(__linux__)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
#elif defined(__APPLE__)
    int count = 0;
    size_t len = sizeof(count);
    if (sysctlbyname("hw.logicalcpu", &count, &len, nullptr, 0) != 0) {
        count = 1;
    }
    return count;
#else
    return 1;
#endif
}

/// CPU relax hint for busy-spin loops.
/// x86: PAUSE (avoid memory-order violation pipeline clears).
/// ARM: YIELD (hint to CPU that we're spinning).
inline void cpuRelax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

}  // namespace utils
}  // namespace chronos

// common.hpp — shared set-up for the side experiments in this folder.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__linux__)
#  include <sched.h>
#endif

#include "harness.hpp"
#include "union_find.hpp"

namespace exp_common {

using Clock = std::chrono::steady_clock;

inline void pin(int cpu) {
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
#else
    (void)cpu;
#endif
}

// Same opt-out as uf_bench (see disable_power_throttling in bench/bench.cpp).
inline bool disable_power_throttling() {
#if defined(_WIN32) && defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION)
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st) != 0;
#else
    return false;
#endif
}

// The checksum of every run is folded in here. The first version of this
// file discarded run_ops' return value, so the compiler could delete the
// side-effect-free connected() queries, and rank+none looked 35% faster
// (14.4 vs 22.7 ns/op at n = 2^10) than uf_bench measures. It is the same
// pitfall as bench::g_sink guards against (H7).
inline volatile std::uint64_t g_exp_sink = 0;

// Median ns/op of `reps` runs; run r uses seqs[r % seqs.size()] on a fresh
// structure. One untimed warm-up on `warm` first.
template <class UF>
double median_ns_per_op(int n, const std::vector<bench::Op>& warm,
                        const std::vector<std::vector<bench::Op>>& seqs, int reps) {
    { UF w(n); g_exp_sink = g_exp_sink + bench::run_ops(w, warm).checksum; }
    std::vector<double> s;
    for (int r = 0; r < reps; ++r) {
        const auto& ops = seqs[static_cast<std::size_t>(r) % seqs.size()];
        UF u(n);
        const auto t0 = Clock::now();
        const std::uint64_t checksum = bench::run_ops(u, ops).checksum;
        const auto t1 = Clock::now();
        g_exp_sink = g_exp_sink + checksum;
        s.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(ops.size()));
    }
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];
}

}  // namespace exp_common

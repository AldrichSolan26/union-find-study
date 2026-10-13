// harness.hpp — the code the benchmark times, shared with the tests so the
// tests check exactly what gets measured.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "workloads.hpp"

namespace bench {

// Every find() result is folded into this sink. Without it, FindNone has no
// side effects, its result is unused, and the optimiser may delete the whole
// find loop. Building with -DUF_BENCH_NO_SINK reproduces that failure mode
// (see results/FINDINGS.md, "validity checks").
// The sink is NOT part of the checksum: different linking policies pick
// different roots, and the checksum must be equal across variants.
inline volatile std::uint64_t g_sink = 0;

struct RunResult {
    std::uint64_t checksum;   // identical for every variant on the same input
    std::uint64_t ops;        // operations actually executed
};

// Run an op sequence. Checksum = merges + "yes" answers + #finds + components.
template <class UF>
RunResult run_ops(UF& u, const std::vector<Op>& ops) {
    std::uint64_t sum = 0;
    std::uint64_t roots = 0;
    for (const auto& op : ops) {
        switch (op.type) {
            case OpType::Unite:     sum += u.unite(op.a, op.b); break;
            case OpType::Connected: sum += u.connected(op.a, op.b); break;
            case OpType::Find:      roots += static_cast<std::uint64_t>(u.find(op.a)); ++sum; break;
        }
    }
#ifndef UF_BENCH_NO_SINK
    g_sink = g_sink + roots;
#else
    (void)roots;
#endif
    return {sum + static_cast<std::uint64_t>(u.components()), ops.size()};
}

// Kruskal's MST. Checksum = total weight (the MST weight is unique, so every
// variant must agree). Stops once n - 1 edges are taken; ops counts only the
// edges actually examined, so ns/op is per unite() call made.
template <class UF>
RunResult run_kruskal(UF& u, const std::vector<Edge>& edges) {
    std::uint64_t weight = 0;
    std::uint64_t examined = 0;
    int taken = 0;
    const int need = u.n() - 1;
    for (const auto& e : edges) {
        if (taken >= need) break;
        ++examined;
        if (u.unite(e.u, e.v)) {
            weight += e.w;
            ++taken;
        }
    }
    return {weight, examined};
}

template <class UF>
RunResult run_workload(UF& u, const Workload& w) {
    return w.is_kruskal() ? run_kruskal(u, w.edges) : run_ops(u, w.ops);
}

// Shape of the final forest, in O(n): each node's depth is computed once,
// reusing the depths of nodes already seen higher up the same path.
struct ForestShape {
    int max_depth = 0;
    double mean_depth = 0.0;
};

inline ForestShape forest_shape(const std::vector<int>& p) {
    const int n = static_cast<int>(p.size());
    std::vector<int> depth(static_cast<std::size_t>(n), -1);
    std::vector<int> path;
    ForestShape s;
    double total = 0.0;
    for (int x = 0; x < n; ++x) {
        int cur = x;
        while (depth[cur] < 0 && p[cur] != cur) { path.push_back(cur); cur = p[cur]; }
        if (depth[cur] < 0) depth[cur] = 0;          // an unvisited root
        int d = depth[cur];
        while (!path.empty()) { depth[path.back()] = ++d; path.pop_back(); }
        s.max_depth = std::max(s.max_depth, depth[x]);
        total += depth[x];
    }
    s.mean_depth = n ? total / n : 0.0;
    return s;
}

}  // namespace bench

// workloads.hpp — operation sequences for the benchmark and the tests.
//
// All generation (random numbers, shuffling, sorting edges) happens BEFORE
// timing, so the harness measures only Union-Find work.
//
// Reproducibility: std::mt19937_64's output is fixed by the C++ standard, but
// std::uniform_int_distribution and std::shuffle are not (libstdc++, libc++
// and MSVC give different sequences). So bounded() and shuffle() below are
// written out by hand, and every workload is bit-identical on every platform.
//
// Workloads (n elements each):
//   random          2n ops, half unite / half connected, uniform endpoints.
//                   The "typical" case; memory access is scattered.
//   chain           unite(i, i+1) for all i, then n finds on uniform nodes.
//                   Adversarial for naive linking: builds a path of length
//                   n - 1. Easy for rank/size linking (it builds a star).
//   binomial        unite(i, i+s) for s = 1, 2, 4, ...; then n uniform finds.
//                   Adversarial for rank/size linking: every merge is a tie,
//                   which builds binomial trees of height exactly log2 n.
//   local           like random, but b is within kLocalWindow of a (mod n).
//   local-shuffled  the SAME op sequence as local with element ids relabelled
//                   by a random permutation. Every policy depends only on
//                   argument order, never on id values, so the forest is
//                   isomorphic and the algorithmic work is IDENTICAL (tested);
//                   only the memory addresses differ. This isolates locality.
//   kruskal         Kruskal's MST on a random multigraph with 4n edges plus a
//                   heavy spanning path (so it is always connected).

#pragma once

#include <algorithm>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bench {

enum class OpType : std::uint8_t { Unite, Connected, Find };

struct Op {
    OpType type;
    int a;
    int b;
};

struct Edge {
    int u, v;
    std::uint32_t w;
};

struct Workload {
    std::string name;
    std::vector<Op> ops;       // used by every workload except kruskal
    std::vector<Edge> edges;   // kruskal only
    bool is_kruskal() const { return name == "kruskal"; }
};

// One 64-byte cache line holds 16 ints.
constexpr int kLocalWindow = 16;

using Rng = std::mt19937_64;

// Uniform integer in [0, bound), bound < 2^32. Lemire's multiply-shift; the
// bias is below 2^-32 per draw, far under anything the study can detect.
inline int bounded(Rng& rng, std::uint64_t bound) {
    return static_cast<int>(((rng() >> 32) * bound) >> 32);
}

template <class T>
void shuffle(std::vector<T>& v, Rng& rng) {      // Fisher-Yates
    for (std::size_t i = v.size(); i > 1; --i)
        std::swap(v[i - 1], v[static_cast<std::size_t>(bounded(rng, i))]);
}

inline std::vector<Op> make_random(int n, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<Op> ops;
    ops.reserve(2 * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        ops.push_back({OpType::Unite, bounded(rng, n), bounded(rng, n)});
        ops.push_back({OpType::Connected, bounded(rng, n), bounded(rng, n)});
    }
    shuffle(ops, rng);
    return ops;
}

inline std::vector<Op> make_chain(int n, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<Op> ops;
    ops.reserve(2 * static_cast<std::size_t>(n));
    for (int i = 0; i + 1 < n; ++i) ops.push_back({OpType::Unite, i, i + 1});
    for (int i = 0; i < n; ++i) ops.push_back({OpType::Find, bounded(rng, n), 0});
    return ops;
}

// Round s merges block [i, i+s) with block [i+s, i+2s). Both are roots of
// equal rank (and equal size), so each merge is a tie and the height grows by
// one per round. Works for any n; the bound is tight when n is a power of 2.
inline std::vector<Op> make_binomial(int n, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<Op> ops;
    ops.reserve(2 * static_cast<std::size_t>(n));
    for (long long s = 1; s < n; s *= 2)
        for (long long i = 0; i + s < n; i += 2 * s)
            ops.push_back({OpType::Unite, static_cast<int>(i), static_cast<int>(i + s)});
    for (int i = 0; i < n; ++i) ops.push_back({OpType::Find, bounded(rng, n), 0});
    return ops;
}

inline std::vector<Op> make_local(int n, std::uint64_t seed) {
    Rng rng(seed);
    const int w = std::min(kLocalWindow, std::max(n - 1, 1));
    std::vector<Op> ops;
    ops.reserve(2 * static_cast<std::size_t>(n));
    for (int i = 0; i < 2 * n; ++i) {
        const int a = bounded(rng, n);
        const int b = static_cast<int>((static_cast<long long>(a) + 1 + bounded(rng, w)) % n);
        ops.push_back({(i % 2 == 0) ? OpType::Unite : OpType::Connected, a, b});
    }
    return ops;
}

// Relabel every element id in ops by one random permutation of [0, n).
inline std::vector<Op> relabel(std::vector<Op> ops, int n, std::uint64_t seed) {
    Rng rng(seed ^ 0x9E3779B97F4A7C15ULL);
    std::vector<int> perm(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) perm[i] = i;
    shuffle(perm, rng);
    for (Op& op : ops) {
        op.a = perm[op.a];
        if (op.type != OpType::Find) op.b = perm[op.b];
    }
    return ops;
}

inline std::vector<Edge> make_kruskal_edges(int n, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<Edge> edges;
    edges.reserve(5 * static_cast<std::size_t>(n));
    for (int i = 0; i < 4 * n; ++i) {
        const int u = bounded(rng, n);
        const int v = bounded(rng, n);
        edges.push_back({u, v, static_cast<std::uint32_t>(bounded(rng, 1'000'001))});
    }
    for (int i = 0; i + 1 < n; ++i) edges.push_back({i, i + 1, 2'000'000});
    // Total order (ties broken by endpoints) so the sorted sequence does not
    // depend on the std::sort implementation.
    std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
        if (x.w != y.w) return x.w < y.w;
        if (x.u != y.u) return x.u < y.u;
        return x.v < y.v;
    });
    return edges;
}

inline const std::vector<std::string>& workload_names() {
    static const std::vector<std::string> names = {
        "random", "chain", "binomial", "local", "local-shuffled", "kruskal"};
    return names;
}

inline Workload make_workload(const std::string& name, int n, std::uint64_t seed) {
    if (n < 1) throw std::invalid_argument("workload size must be >= 1");
    Workload w{name, {}, {}};
    if (name == "random") w.ops = make_random(n, seed);
    else if (name == "chain") w.ops = make_chain(n, seed);
    else if (name == "binomial") w.ops = make_binomial(n, seed);
    else if (name == "local") w.ops = make_local(n, seed);
    else if (name == "local-shuffled") w.ops = relabel(make_local(n, seed), n, seed);
    else if (name == "kruskal") w.edges = make_kruskal_edges(n, seed);
    else throw std::invalid_argument("unknown workload: " + name);
    return w;
}

}  // namespace bench

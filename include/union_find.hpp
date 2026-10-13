// union_find.hpp — Disjoint-set (Union-Find) with pluggable policies.
//
// The structure is split into independent compile-time choices so every
// combination can be benchmarked:
//
//   Link policy  : how two roots are joined      (Naive, ByRank, BySize)
//   Find policy  : what find() does to the path  (None, Full, Halving, Splitting)
//   Stats policy : instrumentation               (NoStats, CountStats)
//
// All three are template parameters, so there is no virtual dispatch in the
// hot path. With NoStats every counter call is an empty inline function and
// compiles away, so the timed code is exactly the algorithm. With CountStats
// the same code also counts parent-array reads and writes, which lets the
// benchmark separate "how much work" from "how long the work took".
//
// Core invariant (all variants):
//   parent[x] == x  <=>  x is the root (canonical representative) of its set,
//   and following parent pointers from any x reaches its root in finitely
//   many steps (the parent graph is a forest; no cycles).
//
//   * Established by the constructor: every element starts as its own root.
//   * Preserved by link(): it only ever writes parent[r] for a ROOT r, and
//     only to point at a different root, so no cycle can be created.
//     unite() guarantees this by calling link only on find() results, and
//     only when they differ.
//   * Preserved by every find policy: compression only re-points a node to
//     an ancestor on its own root path, which keeps the root the same.
//   * Relied on by every find loop: "walk until parent[x] == x" terminates
//     only because the walk cannot cycle.

#pragma once

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace uf {

// ---------------------------------------------------------------------------
// Stats policies. Counters are named after what they cost in memory traffic,
// because that (not instruction count) is what dominates at large n.
// ---------------------------------------------------------------------------

struct NoStats {
    static constexpr bool enabled = false;
    void read() {}
    void write() {}
};

struct CountStats {
    static constexpr bool enabled = true;
    std::uint64_t finds = 0;    // calls to find (unite makes two)
    std::uint64_t path = 0;     // sum over finds of depth(x) BEFORE the find
    std::uint64_t reads = 0;    // parent-array loads
    std::uint64_t writes = 0;   // parent-array stores made by compression
    void read() { ++reads; }
    void write() { ++writes; }
};

// ---------------------------------------------------------------------------
// Find policies. Each takes the parent array and returns the root of x.
// All are ITERATIVE: a recursive find on a naive-linked chain of ~1e5 nodes
// overflows the call stack (tested in test_deep_chain).
// ---------------------------------------------------------------------------

// No compression: just walk to the root. Cost O(depth), structure unchanged.
struct FindNone {
    static constexpr const char* name = "none";
    template <class S>
    static int find(std::vector<int>& p, int x, S& s) {
        for (;;) {
            const int px = p[x]; s.read();
            if (px == x) return x;
            x = px;
        }
    }
};

// Full path compression, two passes: locate the root, then point every node
// on the path directly at it. Afterwards every node on the path has depth 1.
struct FindFull {
    static constexpr const char* name = "full";
    template <class S>
    static int find(std::vector<int>& p, int x, S& s) {
        int root = x;
        for (;;) {                                  // pass 1: find the root
            const int pr = p[root]; s.read();
            if (pr == root) break;
            root = pr;
        }
        for (;;) {                                  // pass 2: re-point the path
            const int next = p[x]; s.read();
            if (next == root) return root;          // also covers x == root
            p[x] = root; s.write();
            x = next;
        }
    }
};

// Path halving, one pass: every OTHER node on the path is re-pointed to its
// grandparent, and the walk jumps to that grandparent. Halves the path.
struct FindHalving {
    static constexpr const char* name = "halving";
    template <class S>
    static int find(std::vector<int>& p, int x, S& s) {
        for (;;) {
            const int px = p[x]; s.read();
            if (px == x) return x;
            const int g = p[px]; s.read();          // safe at the top: p[root]
            p[x] = g; s.write();                    // == root, so a child of the
            x = g;                                  // root re-points to the root
        }
    }
};

// Path splitting, one pass: EVERY node on the path is re-pointed to its
// grandparent, and the walk steps to the OLD parent.
struct FindSplitting {
    static constexpr const char* name = "splitting";
    template <class S>
    static int find(std::vector<int>& p, int x, S& s) {
        for (;;) {
            const int px = p[x]; s.read();
            if (px == x) return x;
            const int g = p[px]; s.read();
            p[x] = g; s.write();
            x = px;
        }
    }
};

// ---------------------------------------------------------------------------
// Link policies. link(p, a, b) is only ever called with two DISTINCT ROOTS
// and returns the root of the merged set. On a tie, `a` stays the root, so
// the result depends only on argument order, never on the element ids.
// ---------------------------------------------------------------------------

// Naive: always hang a under b. Adversarial inputs build a path of length n.
struct LinkNaive {
    static constexpr const char* name = "naive";
    void init(int) {}
    int link(std::vector<int>& p, int a, int b) {
        p[a] = b;
        return b;
    }
};

// Union by rank. Invariants (checked in the tests):
//   * for any non-root x, rank[p[x]] > rank[x];
//   * a root of rank r has at least 2^r elements in its set,
// so rank <= log2 n and, without compression, height <= log2 n.
// Rank fits in one byte: log2 of any int-sized n is below 32.
struct LinkByRank {
    static constexpr const char* name = "rank";
    std::vector<std::uint8_t> rank;
    void init(int n) { rank.assign(static_cast<std::size_t>(n), 0); }
    int link(std::vector<int>& p, int a, int b) {
        if (rank[a] < rank[b]) std::swap(a, b);   // a is now the taller tree
        p[b] = a;
        if (rank[a] == rank[b]) ++rank[a];        // only equal ranks grow
        return a;
    }
};

// Union by size: hang the smaller tree under the larger. Same log2 n height
// bound: a node's depth grows only when its set at least doubles.
struct LinkBySize {
    static constexpr const char* name = "size";
    std::vector<int> size;
    void init(int n) { size.assign(static_cast<std::size_t>(n), 1); }
    int link(std::vector<int>& p, int a, int b) {
        if (size[a] < size[b]) std::swap(a, b);
        p[b] = a;
        size[a] += size[b];
        return a;
    }
};

// ---------------------------------------------------------------------------
// The structure itself.
// ---------------------------------------------------------------------------
template <class Link, class Find, class Stats = NoStats>
class UnionFind {
public:
    using link_type = Link;
    using find_type = Find;
    using stats_type = Stats;

    // n == 0 is allowed (an empty structure); n < 0 is an error.
    explicit UnionFind(int n) : components_(n) {
        if (n < 0) throw std::invalid_argument("UnionFind: n must be >= 0");
        parent_.resize(static_cast<std::size_t>(n));
        std::iota(parent_.begin(), parent_.end(), 0);   // everyone is a root
        link_.init(n);
    }

    // Every public operation validates its arguments: one well-predicted
    // compare, identical for every variant, so comparisons stay fair.
    int find(int x) {
        check(x);
        return root_of(x);
    }

    // Returns true if a merge happened, false if already in the same set.
    bool unite(int a, int b) {
        check(a);
        check(b);
        const int ra = root_of(a);
        const int rb = root_of(b);
        if (ra == rb) return false;     // without this, link(r, r) would be
                                        // called: p[r] = r is harmless, but
                                        // rank/size inflate and the component
                                        // count goes wrong
        link_.link(parent_, ra, rb);    // MUST pass roots, never a and b
        --components_;
        return true;
    }

    bool connected(int a, int b) {
        check(a);
        check(b);
        return root_of(a) == root_of(b);
    }

    int components() const { return components_; }
    int n() const { return static_cast<int>(parent_.size()); }

    // Read-only access for tests and instrumentation.
    const std::vector<int>& parents() const { return parent_; }
    const Link& link_policy() const { return link_; }
    const Stats& stats() const { return stats_; }

    // Depth of x without modifying the structure (for measuring tree shape).
    int depth(int x) const {
        check(x);
        int d = 0;
        while (parent_[x] != x) { x = parent_[x]; ++d; }
        return d;
    }

    static std::string variant_name() {
        return std::string(Link::name) + "+" + Find::name;
    }

private:
    void check(int x) const {
        if (x < 0 || static_cast<std::size_t>(x) >= parent_.size())
            throw std::out_of_range("UnionFind: element " + std::to_string(x) +
                                    " not in [0, " + std::to_string(parent_.size()) + ")");
    }

    int root_of(int x) {
        if constexpr (Stats::enabled) {          // counting runs only
            ++stats_.finds;
            stats_.path += static_cast<std::uint64_t>(depth(x));
        }
        return Find::find(parent_, x, stats_);
    }

    std::vector<int> parent_;
    Link link_;
    Stats stats_;
    int components_;
};

}  // namespace uf

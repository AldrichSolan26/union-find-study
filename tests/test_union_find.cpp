// test_union_find.cpp — correctness tests for all 12 variants, the
// instrumentation, and the benchmark workloads.
//
// No external framework: each check prints the variant and the failure, and
// the program exits non-zero if anything failed.
//
// What is checked:
//   1. Edge cases: n = 0, n = 1, self-union, repeated union, out-of-range
//      arguments and negative n throw.
//   2. Random operations against a brute-force oracle.
//   3. Structural invariants after every batch of operations:
//        - every node reaches a root (no cycles),
//        - rank variants: rank[parent[x]] > rank[x] for non-roots, and
//          |set(r)| >= 2^rank[r] for roots,
//        - size variants: size[root] equals the true set size.
//   4. Height bound: rank/size linking without compression keeps depth
//      <= log2 n, and the binomial workload makes it exactly log2 n.
//   5. Adversarial chain: naive linking really does build depth n - 1.
//   6. What each find policy does to a path (the claims in union_find.hpp).
//   7. Iterative finds survive a 2^20-node chain (no stack overflow).
//   8. Instrumentation: counted == uncounted behaviour; counters are exact.
//   9. Workloads: local and local-shuffled do identical work; Kruskal's MST
//      weight matches an independent Prim's algorithm; every variant gets the
//      same checksum on every workload.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "harness.hpp"
#include "variants.hpp"
#include "workloads.hpp"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, variant, msg)                                              \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL [%s] %s (line %d)\n", std::string(variant).c_str(), \
                        (msg), __LINE__);                                      \
        }                                                                      \
    } while (0)

template <class E, class F>
bool throws(F&& f) {
    try { f(); } catch (const E&) { return true; } catch (...) { return false; }
    return false;
}

// Brute-force oracle: label[x] = id of x's set. unite relabels in O(n).
struct Oracle {
    std::vector<int> label;
    int components;
    explicit Oracle(int n) : label(n), components(n) {
        for (int i = 0; i < n; ++i) label[i] = i;
    }
    bool unite(int a, int b) {
        int la = label[a], lb = label[b];
        if (la == lb) return false;
        for (int& l : label) if (l == lb) l = la;
        --components;
        return true;
    }
    bool connected(int a, int b) const { return label[a] == label[b]; }
};

template <class UF>
void check_invariants(const UF& u, const std::string& v) {
    const auto& p = u.parents();
    const int n = u.n();

    // Every node reaches a root within n steps (so there is no cycle).
    bool all_reach_root = true;
    std::vector<int> set_size(n, 0);
    for (int x = 0; x < n && all_reach_root; ++x) {
        int cur = x, steps = 0;
        while (p[cur] != cur && steps <= n) { cur = p[cur]; ++steps; }
        if (steps > n) all_reach_root = false;
        else ++set_size[cur];
    }
    CHECK(all_reach_root, v, "a node does not reach a root (cycle?)");
    if (!all_reach_root) return;

    using L = typename UF::link_type;
    if constexpr (std::is_same_v<L, uf::LinkByRank>) {
        const auto& rank = u.link_policy().rank;
        bool ok = true, big_enough = true;
        for (int x = 0; x < n; ++x) {
            if (p[x] != x && !(rank[p[x]] > rank[x])) ok = false;
            if (p[x] == x && set_size[x] < (1LL << rank[x])) big_enough = false;
        }
        CHECK(ok, v, "rank invariant rank[parent[x]] > rank[x] violated");
        CHECK(big_enough, v, "a root of rank r has fewer than 2^r elements");
    }
    if constexpr (std::is_same_v<L, uf::LinkBySize>) {
        const auto& size = u.link_policy().size;
        bool ok = true;
        for (int x = 0; x < n; ++x)
            if (p[x] == x && size[x] != set_size[x]) ok = false;
        CHECK(ok, v, "size[root] differs from the true set size");
    }
}

template <class UF>
void test_edge_cases(const std::string& v) {
    UF empty(0);
    CHECK(empty.n() == 0 && empty.components() == 0, v, "n=0: not empty");
    CHECK(throws<std::out_of_range>([&] { empty.find(0); }), v, "n=0: find(0) did not throw");

    UF one(1);
    CHECK(one.find(0) == 0, v, "n=1: find(0) != 0");
    CHECK(!one.unite(0, 0), v, "n=1: self-union reported a merge");
    CHECK(one.components() == 1, v, "n=1: components != 1");

    UF u(5);
    CHECK(u.unite(0, 1), v, "first unite(0,1) should merge");
    CHECK(!u.unite(1, 0), v, "repeated unite(1,0) should not merge");
    CHECK(!u.unite(2, 2), v, "self-union should not merge");
    CHECK(u.components() == 4, v, "components after one merge != 4");
    CHECK(u.connected(0, 1) && !u.connected(0, 2), v, "connectivity wrong");
    CHECK(u.connected(3, 3), v, "an element is not connected to itself");

    CHECK(throws<std::out_of_range>([&] { u.find(-1); }), v, "find(-1) did not throw");
    CHECK(throws<std::out_of_range>([&] { u.find(5); }), v, "find(n) did not throw");
    CHECK(throws<std::out_of_range>([&] { u.unite(0, 5); }), v, "unite(0, n) did not throw");
    CHECK(throws<std::out_of_range>([&] { u.connected(-1, 0); }), v, "connected(-1, 0) did not throw");
    CHECK(u.components() == 4, v, "a rejected call changed the structure");
    CHECK(throws<std::invalid_argument>([] { UF bad(-1); }), v, "negative n did not throw");
}

template <class UF>
void test_against_oracle(const std::string& v) {
    for (unsigned seed = 1; seed <= 20; ++seed) {
        const int n = 1 + static_cast<int>(seed * 37 % 300);
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> pick(0, n - 1);
        UF u(n);
        Oracle o(n);
        bool ok = true;
        for (int step = 0; step < 4 * n; ++step) {
            int a = pick(rng), b = pick(rng);
            switch (rng() % 3) {
                case 0: if (u.unite(a, b) != o.unite(a, b)) ok = false; break;
                case 1: if (u.connected(a, b) != o.connected(a, b)) ok = false; break;
                default: if (o.label[u.find(a)] != o.label[a]) ok = false; break;  // root is in a's set
            }
            if (step % 50 == 0) check_invariants(u, v);
        }
        CHECK(ok, v, "disagrees with brute-force oracle");
        CHECK(u.components() == o.components, v, "component count mismatch");
        check_invariants(u, v);
    }
}

template <class UF>
void test_height_bound(const std::string& v) {
    using L = typename UF::link_type;
    using F = typename UF::find_type;
    // Only meaningful with balanced linking and no compression, where
    // depth is exactly what linking produced.
    if constexpr (!std::is_same_v<L, uf::LinkNaive> && std::is_same_v<F, uf::FindNone>) {
        const int log_n = 12, n = 1 << log_n;
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> pick(0, n - 1);
        UF u(n);
        for (int i = 0; i < 3 * n; ++i) u.unite(pick(rng), pick(rng));
        int max_depth = 0;
        for (int x = 0; x < n; ++x) max_depth = std::max(max_depth, u.depth(x));
        CHECK(max_depth <= log_n, v, "random unions: depth exceeds log2(n)");

        // The bound is tight: the binomial workload reaches it exactly.
        UF b(n);
        bench::run_ops(b, bench::make_binomial(n, 1));
        CHECK(bench::forest_shape(b.parents()).max_depth == log_n, v,
              "binomial workload should reach depth exactly log2(n)");
        check_invariants(b, v);
    }
}

template <class UF>
void test_chain(const std::string& v) {
    using L = typename UF::link_type;
    if constexpr (std::is_same_v<L, uf::LinkNaive>) {
        // Building the chain only ever finds roots, so no find policy can
        // compress it: every naive variant builds depth n - 1.
        const int n = 1000;
        UF u(n);
        for (int i = 0; i + 1 < n; ++i) u.unite(i, i + 1);
        CHECK(u.depth(0) == n - 1, v, "naive chain should have depth n-1");
    }
}

// One find(0) on a chain 0 -> 1 -> ... -> d. Checks exactly what each policy
// promises in union_find.hpp.
template <class UF>
void test_compression_shape(const std::string& v) {
    if constexpr (std::is_same_v<typename UF::link_type, uf::LinkNaive>) {
        using F = typename UF::find_type;
        const int n = 1001, d = n - 1;
        UF u(n);
        for (int i = 0; i + 1 < n; ++i) u.unite(i, i + 1);
        CHECK(u.find(0) == d, v, "find(0) on the chain returned the wrong root");
        const auto& p = u.parents();
        bool ok = true;
        if constexpr (std::is_same_v<F, uf::FindNone>) {
            for (int i = 0; i < d; ++i) ok &= p[i] == i + 1;
            CHECK(ok, v, "none: the path must be unchanged");
        } else if constexpr (std::is_same_v<F, uf::FindFull>) {
            for (int i = 0; i < d; ++i) ok &= p[i] == d;
            CHECK(ok, v, "full: every node on the path must point at the root");
        } else if constexpr (std::is_same_v<F, uf::FindHalving>) {
            for (int i = 0; i < d; ++i) ok &= p[i] == (i % 2 == 0 ? std::min(i + 2, d) : i + 1);
            CHECK(ok, v, "halving: even positions -> grandparent, odd unchanged");
            CHECK(u.depth(0) == (d + 1) / 2, v, "halving: depth(0) should halve");
        } else {
            for (int i = 0; i < d; ++i) ok &= p[i] == std::min(i + 2, d);
            CHECK(ok, v, "splitting: every node on the path -> its grandparent");
            CHECK(u.depth(0) == (d + 1) / 2, v, "splitting: depth(0) should halve");
        }
    }
}

template <class UF>
void test_deep_chain(const std::string& v) {
    using L = typename UF::link_type;
    using F = typename UF::find_type;
    if constexpr (std::is_same_v<L, uf::LinkNaive> && !std::is_same_v<F, uf::FindNone>) {
        const int n = 1 << 20;   // a recursive find would need 2^20 stack frames
        UF u(n);
        for (int i = 0; i + 1 < n; ++i) u.unite(i, i + 1);
        CHECK(u.find(0) == n - 1, v, "deep chain: wrong root");
        CHECK(u.connected(0, n - 1), v, "deep chain: ends not connected");
    }
}

template <class UF>
void test_stats(const std::string& v) {
    using Counted = uf::UnionFind<typename UF::link_type, typename UF::find_type, uf::CountStats>;
    // Same operations -> same answers and the same final forest.
    const int n = 2000;
    const auto ops = bench::make_random(n, 7);
    UF plain(n);
    Counted counted(n);
    const auto r1 = bench::run_ops(plain, ops);
    const auto r2 = bench::run_ops(counted, ops);
    CHECK(r1.checksum == r2.checksum, v, "instrumentation changed the answers");
    CHECK(plain.parents() == counted.parents(), v, "instrumentation changed the forest");
    CHECK(counted.stats().finds == 2 * ops.size(), v, "every unite/connected makes 2 finds");

    // Exact counts on a known path of depth d.
    if constexpr (std::is_same_v<typename UF::link_type, uf::LinkNaive>) {
        const int m = 101, d = m - 1;
        Counted c(m);
        for (int i = 0; i + 1 < m; ++i) c.unite(i, i + 1);
        const uf::CountStats before = c.stats();
        c.find(0);
        const auto reads = c.stats().reads - before.reads;
        const auto writes = c.stats().writes - before.writes;
        CHECK(c.stats().path - before.path == static_cast<std::uint64_t>(d), v, "path should equal depth");
        using F = typename UF::find_type;
        if constexpr (std::is_same_v<F, uf::FindNone>) {
            CHECK(reads == d + 1u && writes == 0, v, "none: d+1 reads, 0 writes");
        } else if constexpr (std::is_same_v<F, uf::FindFull>) {
            CHECK(reads == 2u * d + 1 && writes == d - 1u, v, "full: 2d+1 reads, d-1 writes");
        } else if constexpr (std::is_same_v<F, uf::FindHalving>) {
            CHECK(writes == d / 2u && reads == 2u * (d / 2) + 1, v, "halving: d/2 iterations");
        } else {
            CHECK(writes == static_cast<std::uint64_t>(d) && reads == 2u * d + 1, v,
                  "splitting: d iterations");
        }
    }
}

// local and local-shuffled are the same operations under a relabelling, so
// every counter must match exactly. This is the premise of the locality
// experiment, so it is tested rather than assumed.
template <class UF>
void test_relabel_isomorphic(const std::string& v) {
    using Counted = uf::UnionFind<typename UF::link_type, typename UF::find_type, uf::CountStats>;
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        const int n = 5000;
        const auto a = bench::make_workload("local", n, seed);
        const auto b = bench::make_workload("local-shuffled", n, seed);
        Counted ua(n), ub(n);
        const auto ra = bench::run_workload(ua, a);
        const auto rb = bench::run_workload(ub, b);
        const auto &sa = ua.stats(), &sb = ub.stats();
        CHECK(ra.checksum == rb.checksum, v, "relabelling changed the answers");
        CHECK(sa.reads == sb.reads && sa.writes == sb.writes && sa.path == sb.path, v,
              "relabelling changed the algorithmic work");
        CHECK(bench::forest_shape(ua.parents()).max_depth == bench::forest_shape(ub.parents()).max_depth,
              v, "relabelling changed the forest shape");
    }
}

// Independent check of run_kruskal: Prim's algorithm with a binary heap.
std::uint64_t prim_mst_weight(int n, const std::vector<bench::Edge>& edges) {
    std::vector<std::vector<std::pair<int, std::uint32_t>>> adj(n);
    for (const auto& e : edges) {
        adj[e.u].push_back({e.v, e.w});
        adj[e.v].push_back({e.u, e.w});
    }
    using Item = std::pair<std::uint64_t, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    std::vector<bool> in(n, false);
    std::uint64_t total = 0;
    pq.push({0, 0});
    while (!pq.empty()) {
        auto [w, x] = pq.top();
        pq.pop();
        if (in[x]) continue;
        in[x] = true;
        total += w;
        for (auto [y, wy] : adj[x]) if (!in[y]) pq.push({wy, y});
    }
    return total;
}

void test_workloads() {
    const std::string v = "workloads";
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        const int n = 300 + static_cast<int>(seed) * 17;
        const auto edges = bench::make_kruskal_edges(n, seed);
        uf::UnionFind<uf::LinkByRank, uf::FindFull> u(n);
        const auto r = bench::run_kruskal(u, edges);
        CHECK(u.components() == 1, v, "kruskal graph is not connected");
        CHECK(r.checksum == prim_mst_weight(n, edges), v, "Kruskal MST weight != Prim's");
        CHECK(r.ops <= edges.size(), v, "kruskal examined more edges than exist");
    }
    // Generators are deterministic for a given seed and differ across seeds.
    for (const auto& name : bench::workload_names()) {
        const auto a = bench::make_workload(name, 1024, 3);
        const auto b = bench::make_workload(name, 1024, 3);
        bool same = a.ops.size() == b.ops.size() && a.edges.size() == b.edges.size();
        for (std::size_t i = 0; same && i < a.ops.size(); ++i)
            same = a.ops[i].type == b.ops[i].type && a.ops[i].a == b.ops[i].a && a.ops[i].b == b.ops[i].b;
        for (std::size_t i = 0; same && i < a.edges.size(); ++i)
            same = a.edges[i].u == b.edges[i].u && a.edges[i].w == b.edges[i].w;
        CHECK(same, name, "generator is not deterministic");
        bool in_range = true;
        for (const auto& op : a.ops) in_range &= op.a >= 0 && op.a < 1024 && op.b >= 0 && op.b < 1024;
        CHECK(in_range, name, "generator produced an out-of-range element");
    }
    CHECK(throws<std::invalid_argument>([] { bench::make_workload("nope", 8, 1); }), v,
          "unknown workload name accepted");
    // bounded() stays in range even at the top of the 32-bit range.
    bench::Rng rng(1);
    bool ok = true;
    for (int i = 0; i < 100000; ++i) { const int x = bench::bounded(rng, 7); ok &= x >= 0 && x < 7; }
    CHECK(ok, v, "bounded() out of range");
}

// Every variant must produce the same checksum on every workload, exactly as
// the benchmark requires (it exits non-zero otherwise).
void test_checksums_agree() {
    for (const auto& name : bench::workload_names()) {
        for (std::uint64_t seed = 1; seed <= 2; ++seed) {
            const int n = 1 << 11;
            const auto w = bench::make_workload(name, n, seed);
            std::vector<std::uint64_t> sums;
            uf::for_each_variant([&](auto tag) {
                using UF = typename decltype(tag)::type;
                UF u(n);
                sums.push_back(bench::run_workload(u, w).checksum);
            });
            bool same = true;
            for (auto s : sums) same &= s == sums.front();
            CHECK(same, name, "variants disagree on the checksum");
        }
    }
}

void test_forest_shape() {
    // 0 <- 1 <- 2, 3 alone: depths 0, 1, 2, 0.
    const std::vector<int> p = {0, 0, 1, 3};
    const auto s = bench::forest_shape(p);
    CHECK(s.max_depth == 2 && std::abs(s.mean_depth - 0.75) < 1e-12, "forest_shape", "wrong depths");
}

int main() {
    uf::for_each_variant([](auto tag) {
        using UF = typename decltype(tag)::type;
        const std::string v = UF::variant_name();
        test_edge_cases<UF>(v);
        test_against_oracle<UF>(v);
        test_height_bound<UF>(v);
        test_chain<UF>(v);
        test_compression_shape<UF>(v);
        test_deep_chain<UF>(v);
        test_stats<UF>(v);
        test_relabel_isomorphic<UF>(v);
    });
    test_workloads();
    test_checksums_agree();
    test_forest_shape();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

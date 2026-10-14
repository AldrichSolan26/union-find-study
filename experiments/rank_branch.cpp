// rank_branch.cpp — H5 follow-up: why is union-by-size slightly FASTER than
// union-by-rank at small n?
//
// Hypothesis: rank's "if (rank[a] == rank[b]) ++rank[a]" is an unpredictable
// branch (ties are common), while size's "size[a] += size[b]" has no branch.
// Test: a rank policy whose increment is written branch-free. It builds the
// same trees and gives the same answers; only the code shape differs.
//
//   experiments/rank_branch [cpu]

#include <cstdlib>

#include "common.hpp"

struct LinkByRankBranchless {
    static constexpr const char* name = "rank-branchless";
    std::vector<std::uint8_t> rank;
    void init(int n) { rank.assign(static_cast<std::size_t>(n), 0); }
    int link(std::vector<int>& p, int a, int b) {
        if (rank[a] < rank[b]) std::swap(a, b);
        p[b] = a;
        rank[a] = static_cast<std::uint8_t>(rank[a] + (rank[a] == rank[b]));
        return a;
    }
};

int main(int argc, char** argv) {
    const int cpu = argc > 1 ? std::atoi(argv[1]) : 4;
    exp_common::pin(cpu);
    exp_common::disable_power_throttling();
    std::printf("random workload, find = halving, ns/op (two rounds per n)\n");
    std::printf("%8s %8s %8s %16s\n", "n", "rank", "size", "rank-branchless");
    for (int e : {10, 12, 14, 16, 18}) {
        const int n = 1 << e;
        const int reps = std::max(5, (1 << 19) / (2 * n));
        const auto warm = bench::make_random(n, 4999);
        std::vector<std::vector<bench::Op>> seqs;
        for (int k = 0; k < reps; ++k) seqs.push_back(bench::make_random(n, 5000 + static_cast<unsigned>(k)));
        for (int round = 0; round < 2; ++round)
            std::printf("%8d %8.2f %8.2f %16.2f\n", n,
                        exp_common::median_ns_per_op<uf::UnionFind<uf::LinkByRank, uf::FindHalving>>(n, warm, seqs, reps),
                        exp_common::median_ns_per_op<uf::UnionFind<uf::LinkBySize, uf::FindHalving>>(n, warm, seqs, reps),
                        exp_common::median_ns_per_op<uf::UnionFind<LinkByRankBranchless, uf::FindHalving>>(n, warm, seqs, reps));
    }
}

// replay_vs_fresh.cpp — H8: does replaying one op sequence let the branch
// predictor learn it?
//
// Same n, same workload distribution (random), same variant. (a) one sequence
// replayed for every timed rep; (b) a different, freshly generated sequence
// for every rep. All sequences are generated before timing.
//
//   experiments/replay_vs_fresh [cpu]

#include <cstdlib>

#include "common.hpp"

int main(int argc, char** argv) {
    const int cpu = argc > 1 ? std::atoi(argv[1]) : 4;
    exp_common::pin(cpu);
    exp_common::disable_power_throttling();
    using Full = uf::UnionFind<uf::LinkByRank, uf::FindFull>;
    using None = uf::UnionFind<uf::LinkByRank, uf::FindNone>;
    std::printf("random workload, ns/op (median of 201 reps)\n");
    std::printf("%8s %14s %14s %14s %14s\n", "n", "full replayed", "full fresh", "none replayed", "none fresh");
    for (int e = 10; e <= 16; ++e) {
        const int n = 1 << e, reps = 201;
        const auto warm = bench::make_random(n, 999);
        const std::vector<std::vector<bench::Op>> one{bench::make_random(n, 1)};
        std::vector<std::vector<bench::Op>> many;
        for (int r = 0; r < reps; ++r) many.push_back(bench::make_random(n, 1000 + static_cast<unsigned>(r)));
        std::printf("%8d %14.2f %14.2f %14.2f %14.2f\n", n,
                    exp_common::median_ns_per_op<Full>(n, warm, one, reps),
                    exp_common::median_ns_per_op<Full>(n, warm, many, reps),
                    exp_common::median_ns_per_op<None>(n, warm, one, reps),
                    exp_common::median_ns_per_op<None>(n, warm, many, reps));
    }
}

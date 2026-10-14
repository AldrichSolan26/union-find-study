// throttling.cpp — H9: does sustained load slow later measurements, and does
// a pause restore them? Distinguishes clock/power throttling from leftover
// memory state. Run it twice:
//
//   experiments/throttling [cpu]            # Windows power throttling left on
//   experiments/throttling [cpu] --opt-out  # same opt-out as uf_bench

#include <cstdlib>
#include <cstring>
#include <thread>

#include "common.hpp"

using exp_common::Clock;

double probe(int n, const std::vector<bench::Op>& warm, const std::vector<std::vector<bench::Op>>& seqs) {
    return exp_common::median_ns_per_op<uf::UnionFind<uf::LinkByRank, uf::FindFull>>(n, warm, seqs, 12);
}

volatile std::uint64_t g_sink;

void alu_load(double seconds) {       // pure arithmetic, no memory traffic
    const auto end = Clock::now() + std::chrono::duration<double>(seconds);
    std::uint64_t x = 1;
    while (Clock::now() < end)
        for (int i = 0; i < 100000; ++i) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    g_sink = x;
}

void chase_load(double seconds) {     // what naive+none does: a long dependent pointer chase
    const int n = 1 << 15;
    std::vector<int> p(n);
    for (int i = 0; i < n; ++i) p[i] = (i + 1 < n) ? i + 1 : i;
    const auto end = Clock::now() + std::chrono::duration<double>(seconds);
    std::uint64_t acc = 0;
    while (Clock::now() < end) { int x = 0; while (p[x] != x) x = p[x]; acc += static_cast<std::uint64_t>(x); }
    g_sink = acc;
}

void pause(double seconds) { std::this_thread::sleep_for(std::chrono::duration<double>(seconds)); }

int main(int argc, char** argv) {
    const int cpu = argc > 1 ? std::atoi(argv[1]) : 4;
    const bool opt_out = argc > 2 && !std::strcmp(argv[2], "--opt-out");
    exp_common::pin(cpu);
    if (opt_out) std::printf("power throttling opt-out: %s\n", exp_common::disable_power_throttling() ? "ok" : "FAILED");
    else std::printf("power throttling left on\n");

    const int n = 1 << 15;
    const auto warm = bench::make_random(n, 99);
    std::vector<std::vector<bench::Op>> seqs;
    for (int k = 0; k < 4; ++k) seqs.push_back(bench::make_random(n, 100 + static_cast<unsigned>(k)));
    probe(n, warm, seqs);
    std::printf("random n=2^15 rank+full, ns/op\n");
    std::printf("  baseline                 %.1f\n", probe(n, warm, seqs));
    chase_load(5); std::printf("  after 5 s pointer chase  %.1f\n", probe(n, warm, seqs));
    std::printf("    again                  %.1f\n", probe(n, warm, seqs));
    pause(3);      std::printf("  after 3 s pause          %.1f\n", probe(n, warm, seqs));
    alu_load(5);   std::printf("  after 5 s ALU loop       %.1f\n", probe(n, warm, seqs));
    pause(3);      std::printf("  after 3 s pause          %.1f\n", probe(n, warm, seqs));
}

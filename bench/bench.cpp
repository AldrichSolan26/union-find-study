// bench.cpp — timing harness for all 12 Union-Find variants.
//
// For each (workload, n, seed, variant):
//   1. one COUNTING run (CountStats) on the canonical sequence for that seed:
//      parent-array reads/writes, path lengths and the final forest shape.
//      Untimed. These numbers are deterministic.
//   2. one untimed warm-up run;
//   3. timed runs on a freshly constructed structure (construction is not
//      timed) until at least --min-reps runs AND --min-time-ms of timed work,
//      or --max-reps runs. Median, quartiles and minimum are reported.
//      For short sequences each timed run gets its own freshly generated
//      sequence of the same workload (see SequencePool below).
//
// Every run returns a checksum (see harness.hpp). All variants must agree on
// the canonical sequence, and every timed run must match a reference
// variant's answer for its sequence; otherwise the harness prints
// CHECKSUM MISMATCH and exits non-zero.
//
// Usage:
//   uf_bench [--workload all|NAME[,NAME...]] [--min-exp 10] [--max-exp 20]
//            [--slow-max-exp 15] [--seeds 1[,2,...]] [--min-reps 5]
//            [--max-reps 51] [--min-time-ms 50] [--cpu N] [--no-count]
//            [--replay-same-sequence] [--allow-power-throttling]
//            [--out results/results.csv] [--build-info]
//
// --slow-max-exp caps ONLY naive+none, which is O(n) per find on chain-like
// inputs; above that size its rows are simply omitted.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // the MSVC runtime deprecates std::fopen; it is fine here
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__linux__)
#  include <sched.h>
#  include <fstream>
#endif

#include "harness.hpp"
#include "variants.hpp"
#include "workloads.hpp"

namespace {

struct Config {
    std::vector<std::string> workloads = bench::workload_names();
    std::vector<std::string> variants;   // empty = all 12
    int min_exp = 10;
    int max_exp = 20;
    int slow_max_exp = 15;
    std::vector<std::uint64_t> seeds = {1};
    int min_reps = 5;
    int max_reps = 51;
    double min_time_ms = 50.0;
    int cpu = -1;
    bool count = true;
    bool replay = false;      // validity experiment: time one sequence over and over
    bool allow_throttling = false;  // validity experiment: leave Windows power throttling on
    bool build_info = false;
    std::string out = "results/results.csv";
};

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = s.find(',', start);
        parts.push_back(s.substr(start, comma - start));
        if (comma == std::string::npos) return parts;
        start = comma + 1;
    }
}

[[noreturn]] void usage_error(const std::string& msg) {
    std::fprintf(stderr, "uf_bench: %s (try --help)\n", msg.c_str());
    std::exit(2);
}

int parse_int(const char* s, const char* flag) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0') usage_error(std::string("bad integer for ") + flag + ": " + s);
    return static_cast<int>(v);
}

// Windows 11 "power throttling" (EcoQoS) clocks down a busy process that has
// no foreground window. On the study machine it kicked in after a few seconds
// of sustained load and made every later measurement ~2x slower, so variant
// ORDER changed the results (see results/FINDINGS.md, "validity checks").
// Opting out keeps the core at its normal clocks for the whole run.
bool disable_power_throttling() {
#if defined(_WIN32) && defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION)
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;   // take control of execution-speed throttling, and turn it off
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st) != 0;
#else
    return false;
#endif
}

// Data/unified caches seen by one logical CPU, printed as
// "cache L<level> <bytes>" lines (scripts/run_study.py parses them).
void print_caches(int cpu) {
    if (cpu < 0) cpu = 0;
#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    std::vector<char> buf(len);
    if (!len || !GetLogicalProcessorInformationEx(
                    RelationCache, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()), &len))
        return;
    for (char* p = buf.data(); p < buf.data() + len;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
        const CACHE_RELATIONSHIP& c = e->Cache;
        if (c.Type != CacheInstruction && c.GroupMask.Group == 0 && cpu < 64 &&
            ((c.GroupMask.Mask >> cpu) & 1))
            std::printf("cache L%d %lu\n", static_cast<int>(c.Level), static_cast<unsigned long>(c.CacheSize));
        p += e->Size;
    }
#elif defined(__linux__)
    for (int i = 0; i < 8; ++i) {
        const std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                                "/cache/index" + std::to_string(i) + "/";
        std::ifstream level(dir + "level"), type(dir + "type"), size(dir + "size");
        int lv = 0; std::string ty, sz;
        if (!(level >> lv) || !(type >> ty) || !(size >> sz)) break;
        if (ty == "Instruction") continue;
        long bytes = std::atol(sz.c_str());
        if (sz.back() == 'K') bytes *= 1024; else if (sz.back() == 'M') bytes *= 1024 * 1024;
        std::printf("cache L%d %ld\n", lv, bytes);
    }
#else
    (void)cpu;
#endif
}

void print_build_info(int cpu) {
#if defined(__clang__)
    std::printf("compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler: gcc %s\n", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("compiler: msvc %d\n", _MSC_VER);
#endif
#ifdef NDEBUG
    std::puts("NDEBUG: defined");
#else
    std::puts("NDEBUG: not defined (DEBUG BUILD: timings are meaningless)");
#endif
#if defined(__OPTIMIZE__)
    std::puts("optimised: yes");
#elif defined(__GNUC__)
    std::puts("optimised: NO (built without -O)");
#endif
#ifdef UF_BENCH_NO_SINK
    std::puts("sink: DISABLED (validity experiment build)");
#else
    std::puts("sink: enabled");
#endif
    // Smallest observable step of the clock.
    using Clock = std::chrono::steady_clock;
    double best = 1e18;
    for (int i = 0; i < 1000; ++i) {
        const auto t0 = Clock::now();
        auto t1 = Clock::now();
        while (t1 == t0) t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    std::printf("steady_clock resolution: ~%.0f ns\n", best);
#if defined(_WIN32)
    std::printf("windows power throttling: %s\n", disable_power_throttling() ? "disabled for this process" : "COULD NOT DISABLE");
#endif
    print_caches(cpu);
}

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--workload") {
            const std::string v = next();
            if (v != "all") c.workloads = split(v);
        } else if (arg == "--min-exp") c.min_exp = parse_int(next(), "--min-exp");
        else if (arg == "--max-exp") c.max_exp = parse_int(next(), "--max-exp");
        else if (arg == "--slow-max-exp") c.slow_max_exp = parse_int(next(), "--slow-max-exp");
        else if (arg == "--min-reps") c.min_reps = parse_int(next(), "--min-reps");
        else if (arg == "--max-reps") c.max_reps = parse_int(next(), "--max-reps");
        else if (arg == "--min-time-ms") c.min_time_ms = parse_int(next(), "--min-time-ms");
        else if (arg == "--cpu") c.cpu = parse_int(next(), "--cpu");
        else if (arg == "--no-count") c.count = false;
        else if (arg == "--variants") c.variants = split(next());
        else if (arg == "--replay-same-sequence") c.replay = true;
        else if (arg == "--allow-power-throttling") c.allow_throttling = true;
        else if (arg == "--out") c.out = next();
        else if (arg == "--seeds") {
            c.seeds.clear();
            for (const auto& s : split(next())) c.seeds.push_back(static_cast<std::uint64_t>(parse_int(s.c_str(), "--seeds")));
        } else if (arg == "--build-info") c.build_info = true;
        else if (arg == "--help") {
            std::puts("uf_bench [--workload all|NAME[,NAME...]] [--min-exp N] [--max-exp N]\n"
                      "         [--slow-max-exp N] [--seeds S1,S2,...] [--min-reps N] [--max-reps N]\n"
                      "         [--min-time-ms N] [--cpu N] [--no-count] [--replay-same-sequence]\n"
                      "         [--allow-power-throttling] [--out FILE] [--build-info]\n"
                      "workloads: random chain binomial local local-shuffled kruskal");
            std::exit(0);
        } else usage_error("unknown argument: " + arg);
    }
    for (const auto& w : c.workloads) {
        const auto& names = bench::workload_names();
        if (std::find(names.begin(), names.end(), w) == names.end()) usage_error("unknown workload: " + w);
    }
    // 2^26 elements with 2n ops already needs ~1.6 GB; 2^30 would overflow int ops counts.
    if (c.min_exp < 1 || c.max_exp > 26 || c.max_exp < c.min_exp) usage_error("need 1 <= min-exp <= max-exp <= 26");
    if (c.min_reps < 1 || c.max_reps < c.min_reps) usage_error("need 1 <= min-reps <= max-reps");
    if (c.seeds.empty()) usage_error("need at least one seed");
    return c;
}

// Pin to one logical CPU. On hybrid CPUs (P-cores + E-cores) an unpinned run
// can migrate between core types mid-measurement.
void pin_to_cpu(int cpu) {
    if (cpu < 0) return;
#if defined(_WIN32)
    if (!SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu)) {
        std::fprintf(stderr, "warning: could not pin to CPU %d\n", cpu);
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // Report the core type: a higher EfficiencyClass is a faster core.
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<char> buf(len);
    auto* info = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data());
    if (len && GetSystemCpuSetInformation(info, len, &len, GetCurrentProcess(), 0)) {
        int mine = -1, best = -1;
        for (char* p = buf.data(); p < buf.data() + len;) {
            auto* e = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(p);
            const int cls = e->CpuSet.EfficiencyClass;
            best = std::max(best, cls);
            if (e->CpuSet.LogicalProcessorIndex == cpu) mine = cls;
            p += e->Size;
        }
        std::fprintf(stderr, "pinned to logical CPU %d (efficiency class %d of max %d: %s core)\n",
                     cpu, mine, best, mine == best ? "performance" : "EFFICIENCY");
    }
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: could not pin to CPU %d\n", cpu);
    else
        std::fprintf(stderr, "pinned to logical CPU %d\n", cpu);
#else
    std::fprintf(stderr, "warning: --cpu is not supported on this platform\n");
#endif
}

using Clock = std::chrono::steady_clock;

// The sequences the timed runs use. Replaying ONE short sequence rep after rep
// lets the branch predictor memorise its outcomes: at n = 2^10 that made
// rank+full look 4.7x faster than on an unseen sequence of the same kind (see
// results/FINDINGS.md, "validity checks"). So while one sequence is short,
// every rep gets its own independently generated sequence, with a separate
// one for the warm-up, until the pool covers kPoolOps operations. Above that
// the predictor cannot hold the history, and the canonical sequence is reused.
constexpr std::size_t kPoolOps = std::size_t(1) << 18;

struct SequencePool {
    std::vector<bench::Workload> owned;
    std::vector<const bench::Workload*> seqs;   // seqs[0] is the warm-up
    std::vector<std::uint64_t> expect;          // checksum of each sequence
    std::size_t timed() const { return seqs.size() == 1 ? 1 : seqs.size() - 1; }
    std::size_t index(int rep) const { return seqs.size() == 1 ? 0 : 1 + rep % (seqs.size() - 1); }
};

std::uint64_t derived_seed(std::uint64_t seed, std::size_t k) {
    return seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL * (k + 1);
}

SequencePool make_pool(const bench::Workload& canonical, int n, std::uint64_t seed, const Config& cfg) {
    SequencePool pool;
    const std::size_t len = canonical.is_kruskal() ? canonical.edges.size() : canonical.ops.size();
    const std::size_t want = cfg.replay ? 1 : std::clamp<std::size_t>(kPoolOps / std::max<std::size_t>(len, 1), 1,
                                                                    static_cast<std::size_t>(cfg.max_reps));
    if (want == 1) {
        pool.seqs.push_back(&canonical);
    } else {
        pool.owned.reserve(want + 1);
        for (std::size_t k = 0; k <= want; ++k)
            pool.owned.push_back(bench::make_workload(canonical.name, n, derived_seed(seed, k)));
        for (const auto& w : pool.owned) pool.seqs.push_back(&w);
    }
    for (const auto* w : pool.seqs) {      // reference answers, from one fixed variant
        uf::UnionFind<uf::LinkByRank, uf::FindFull> ref(n);
        pool.expect.push_back(bench::run_workload(ref, *w).checksum);
    }
    return pool;
}

struct Timing {
    double median, p25, p75, min;   // ns per run
    double ns_per_op;               // median over runs of (ns / ops in that run)
    int reps;
    bool consistent;                // every timed run produced the expected checksum
};

double quantile(const std::vector<double>& sorted, double q) {
    const double pos = q * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (pos - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]);
}

template <class UF>
Timing time_variant(const SequencePool& pool, int n, const Config& cfg) {
    bool consistent = true;
    { UF warm(n); consistent &= bench::run_workload(warm, *pool.seqs[0]).checksum == pool.expect[0]; }
    std::vector<double> samples, per_op;
    double total_ms = 0.0;
    while (static_cast<int>(samples.size()) < cfg.min_reps ||
           (total_ms < cfg.min_time_ms && static_cast<int>(samples.size()) < cfg.max_reps)) {
        const std::size_t k = pool.index(static_cast<int>(samples.size()));
        UF u(n);                                    // construction: not timed
        const auto t0 = Clock::now();
        const auto r = bench::run_workload(u, *pool.seqs[k]);
        const auto t1 = Clock::now();
        consistent &= r.checksum == pool.expect[k];
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        samples.push_back(ns);
        per_op.push_back(ns / static_cast<double>(r.ops));
        total_ms += ns / 1e6;
    }
    std::sort(samples.begin(), samples.end());
    std::sort(per_op.begin(), per_op.end());
    return {quantile(samples, 0.5), quantile(samples, 0.25), quantile(samples, 0.75),
            samples.front(), quantile(per_op, 0.5), static_cast<int>(samples.size()), consistent};
}

}  // namespace

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);
    if (cfg.build_info) {
        print_build_info(cfg.cpu);
        return 0;
    }
#if !defined(NDEBUG) || (defined(__GNUC__) && !defined(__OPTIMIZE__))
    std::fprintf(stderr, "WARNING: unoptimised/debug build; timings are meaningless\n");
#endif
#if defined(_WIN32)
    if (cfg.allow_throttling)
        std::fprintf(stderr, "note: Windows power throttling left ON (validity experiment)\n");
    else if (!disable_power_throttling())
        std::fprintf(stderr, "WARNING: could not disable Windows power throttling; timings may drift\n");
#endif
    pin_to_cpu(cfg.cpu);

    std::FILE* out = std::fopen(cfg.out.c_str(), "w");
    if (!out) {
        std::fprintf(stderr, "cannot open %s (does the folder exist?)\n", cfg.out.c_str());
        return 1;
    }
    std::fprintf(out, "workload,variant,link,find,n,seed,ops,reps,sequences,median_ns,p25_ns,p75_ns,min_ns,"
                      "ns_per_op,finds,path_per_find,reads_per_op,writes_per_op,max_depth,"
                      "mean_depth,checksum\n");

    const auto start = Clock::now();
    int mismatches = 0;
    for (const auto& wname : cfg.workloads) {
        for (int e = cfg.min_exp; e <= cfg.max_exp; ++e) {
            const int n = 1 << e;
            for (const std::uint64_t seed : cfg.seeds) {
                const bench::Workload w = bench::make_workload(wname, n, seed);
                const SequencePool pool = make_pool(w, n, seed, cfg);
                std::map<std::string, std::uint64_t> sums;

                uf::for_each_variant([&](auto tag) {
                    using UF = typename decltype(tag)::type;
                    using Counted = uf::UnionFind<typename UF::link_type,
                                                  typename UF::find_type, uf::CountStats>;
                    if (uf::is_unprotected<UF> && e > cfg.slow_max_exp) return;
                    if (!cfg.variants.empty() &&
                        std::find(cfg.variants.begin(), cfg.variants.end(), UF::variant_name()) == cfg.variants.end())
                        return;

                    // 1. Counting run (also fixes the expected checksum).
                    std::uint64_t checksum = 0, ops = 0;
                    uf::CountStats st;
                    bench::ForestShape shape;
                    {
                        Counted c(n);
                        const auto r = bench::run_workload(c, w);
                        checksum = r.checksum;
                        ops = r.ops;
                        if (cfg.count) { st = c.stats(); shape = bench::forest_shape(c.parents()); }
                    }
                    // 2-3. Warm-up and timed runs on the uninstrumented type.
                    const Timing t = time_variant<UF>(pool, n, cfg);
                    if (!t.consistent) {
                        ++mismatches;
                        std::fprintf(stderr, "CHECKSUM MISMATCH (timed run vs reference) %s n=%d seed=%llu %s\n",
                                     wname.c_str(), n, static_cast<unsigned long long>(seed),
                                     UF::variant_name().c_str());
                    }
                    sums[UF::variant_name()] = checksum;

                    const double dops = static_cast<double>(ops);
                    const double finds = static_cast<double>(st.finds);
                    std::fprintf(out, "%s,%s,%s,%s,%d,%llu,%llu,%d,%zu,%.0f,%.0f,%.0f,%.0f,%.4f,",
                                 wname.c_str(), UF::variant_name().c_str(), UF::link_type::name,
                                 UF::find_type::name, n, static_cast<unsigned long long>(seed),
                                 static_cast<unsigned long long>(ops), t.reps,
                                 std::min<std::size_t>(pool.timed(), static_cast<std::size_t>(t.reps)),
                                 t.median, t.p25, t.p75, t.min, t.ns_per_op);
                    if (cfg.count)
                        std::fprintf(out, "%llu,%.4f,%.4f,%.4f,%d,%.4f,",
                                     static_cast<unsigned long long>(st.finds),
                                     finds > 0 ? static_cast<double>(st.path) / finds : 0.0,
                                     static_cast<double>(st.reads) / dops,
                                     static_cast<double>(st.writes) / dops,
                                     shape.max_depth, shape.mean_depth);
                    else
                        std::fprintf(out, ",,,,,,");
                    std::fprintf(out, "%llu\n", static_cast<unsigned long long>(checksum));
                    std::fflush(out);
                });

                // All variants implement the same abstract structure, so they must agree.
                const std::uint64_t ref = sums.begin()->second;
                for (const auto& [name, s] : sums) {
                    if (s != ref) {
                        ++mismatches;
                        std::fprintf(stderr, "CHECKSUM MISMATCH %s n=%d seed=%llu: %s\n",
                                     wname.c_str(), n, static_cast<unsigned long long>(seed), name.c_str());
                    }
                }
                const double secs = std::chrono::duration<double>(Clock::now() - start).count();
                std::fprintf(stderr, "[%7.1fs] done %-14s n=2^%-2d seed=%llu\n", secs, wname.c_str(), e,
                             static_cast<unsigned long long>(seed));
            }
        }
    }

    std::fclose(out);
    std::fprintf(stderr, "wrote %s%s\n", cfg.out.c_str(),
                 mismatches ? " -- WITH CHECKSUM MISMATCHES" : " (all checksums agree)");
    return mismatches == 0 ? 0 : 1;
}

// bench.cpp — timing harness for all 12 Union-Find variants.
//
// For each (workload, n, seed, variant):
//   1. one COUNTING run (CountStats): parent-array reads/writes, path lengths
//      and the final forest shape. Untimed. These numbers are deterministic.
//   2. one untimed warm-up run;
//   3. timed runs on a freshly constructed structure (construction is not
//      timed) until at least --min-reps runs AND --min-time-ms of timed work,
//      or --max-reps runs. Median, quartiles and minimum are reported.
//
// Every run returns a checksum (see harness.hpp). All variants, the counting
// run and every timed run must agree; otherwise the harness prints
// CHECKSUM MISMATCH and exits non-zero.
//
// Usage:
//   uf_bench [--workload all|NAME[,NAME...]] [--min-exp 10] [--max-exp 20]
//            [--slow-max-exp 15] [--seeds 1[,2,...]] [--min-reps 5]
//            [--max-reps 51] [--min-time-ms 50] [--cpu N] [--no-count]
//            [--out results/results.csv] [--build-info]
//
// --slow-max-exp caps ONLY naive+none, which is O(n) per find on chain-like
// inputs; above that size its rows are simply omitted.

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
    int min_exp = 10;
    int max_exp = 20;
    int slow_max_exp = 15;
    std::vector<std::uint64_t> seeds = {1};
    int min_reps = 5;
    int max_reps = 51;
    double min_time_ms = 50.0;
    int cpu = -1;
    bool count = true;
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
        else if (arg == "--out") c.out = next();
        else if (arg == "--seeds") {
            c.seeds.clear();
            for (const auto& s : split(next())) c.seeds.push_back(static_cast<std::uint64_t>(parse_int(s.c_str(), "--seeds")));
        } else if (arg == "--build-info") c.build_info = true;
        else if (arg == "--help") {
            std::puts("uf_bench [--workload all|NAME[,NAME...]] [--min-exp N] [--max-exp N]\n"
                      "         [--slow-max-exp N] [--seeds S1,S2,...] [--min-reps N] [--max-reps N]\n"
                      "         [--min-time-ms N] [--cpu N] [--no-count] [--out FILE] [--build-info]\n"
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

struct Timing {
    double median, p25, p75, min;
    int reps;
    bool consistent;   // every timed run produced the expected checksum
};

double quantile(const std::vector<double>& sorted, double q) {
    const double pos = q * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (pos - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]);
}

template <class UF>
Timing time_variant(const bench::Workload& w, int n, const Config& cfg, std::uint64_t expect) {
    bool consistent = true;
    { UF warm(n); consistent &= bench::run_workload(warm, w).checksum == expect; }
    std::vector<double> samples;
    double total_ms = 0.0;
    while (static_cast<int>(samples.size()) < cfg.min_reps ||
           (total_ms < cfg.min_time_ms && static_cast<int>(samples.size()) < cfg.max_reps)) {
        UF u(n);                                    // construction: not timed
        const auto t0 = Clock::now();
        const auto r = bench::run_workload(u, w);
        const auto t1 = Clock::now();
        consistent &= r.checksum == expect;
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        samples.push_back(ns);
        total_ms += ns / 1e6;
    }
    std::sort(samples.begin(), samples.end());
    return {quantile(samples, 0.5), quantile(samples, 0.25), quantile(samples, 0.75),
            samples.front(), static_cast<int>(samples.size()), consistent};
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
    pin_to_cpu(cfg.cpu);

    std::FILE* out = std::fopen(cfg.out.c_str(), "w");
    if (!out) {
        std::fprintf(stderr, "cannot open %s (does the folder exist?)\n", cfg.out.c_str());
        return 1;
    }
    std::fprintf(out, "workload,variant,link,find,n,seed,ops,reps,median_ns,p25_ns,p75_ns,min_ns,"
                      "ns_per_op,finds,path_per_find,reads_per_op,writes_per_op,max_depth,"
                      "mean_depth,checksum\n");

    const auto start = Clock::now();
    int mismatches = 0;
    for (const auto& wname : cfg.workloads) {
        for (int e = cfg.min_exp; e <= cfg.max_exp; ++e) {
            const int n = 1 << e;
            for (const std::uint64_t seed : cfg.seeds) {
                const bench::Workload w = bench::make_workload(wname, n, seed);
                std::map<std::string, std::uint64_t> sums;

                uf::for_each_variant([&](auto tag) {
                    using UF = typename decltype(tag)::type;
                    using Counted = uf::UnionFind<typename UF::link_type,
                                                  typename UF::find_type, uf::CountStats>;
                    if (uf::is_unprotected<UF> && e > cfg.slow_max_exp) return;

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
                    const Timing t = time_variant<UF>(w, n, cfg, checksum);
                    if (!t.consistent) {
                        ++mismatches;
                        std::fprintf(stderr, "CHECKSUM MISMATCH (timed vs counted) %s n=%d seed=%llu %s\n",
                                     wname.c_str(), n, static_cast<unsigned long long>(seed),
                                     UF::variant_name().c_str());
                    }
                    sums[UF::variant_name()] = checksum;

                    const double dops = static_cast<double>(ops);
                    const double finds = static_cast<double>(st.finds);
                    std::fprintf(out, "%s,%s,%s,%s,%d,%llu,%llu,%d,%.0f,%.0f,%.0f,%.0f,%.4f,",
                                 wname.c_str(), UF::variant_name().c_str(), UF::link_type::name,
                                 UF::find_type::name, n, static_cast<unsigned long long>(seed),
                                 static_cast<unsigned long long>(ops), t.reps, t.median, t.p25,
                                 t.p75, t.min, t.median / dops);
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

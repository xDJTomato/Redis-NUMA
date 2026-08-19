// cxlmemsim_workload_bench.cpp - a NUMAflow-workload-shaped micro-benchmark
// driven directly through CXLMemSim's own C++ device-timing model
// (CXLMemExpander::calculate_latency / calculate_bandwidth from
// external/CXLMemSim/include/cxlendpoint.h), rather than through NUMAflow's
// simplified single-latency/single-bandwidth cost model.
//
// This exists to answer a different question than
// numaflow/eval/report.py's bar charts: "what does CXLMemSim's own
// congestion-aware bandwidth model and cache-state-aware latency model
// say about these access patterns", using the exact same four workload
// shapes (zipf/uniform/hotspot/temporal) NUMAflow's fair-evaluation
// harness uses, so the two are comparable in kind if not in absolute
// units.
//
// Usage matches the CXLMemSim test suite's own pattern (see
// external/CXLMemSim/tests/test_bandwidth_model.cpp): construct a
// CXLMemExpander with real device parameters, register the address range
// as "local" via occupation entries, then call calculate_latency /
// calculate_bandwidth on a (timestamp_ns, address) trace directly --
// no insert()-driven counter state, matching that test's usage.
#include "cxlendpoint.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    double uniform01() { return static_cast<double>(next() >> 11) / static_cast<double>(1ULL << 53); }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
};

std::vector<uint64_t> gen_zipf(Rng &rng, size_t keys, size_t n, double theta = 0.99) {
    std::vector<double> cum(keys);
    double sum = 0.0;
    for (size_t r = 1; r <= keys; r++) {
        sum += 1.0 / std::pow(static_cast<double>(r), theta);
        cum[r - 1] = sum;
    }
    for (auto &c : cum) c /= sum;
    std::vector<uint64_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        double u = rng.uniform01();
        size_t lo = 0, hi = keys - 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (cum[mid] < u) lo = mid + 1; else hi = mid;
        }
        out.push_back(lo);
    }
    return out;
}

std::vector<uint64_t> gen_uniform(Rng &rng, size_t keys, size_t n) {
    std::vector<uint64_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) out.push_back(rng.below(keys));
    return out;
}

std::vector<uint64_t> gen_hotspot(Rng &rng, size_t keys, size_t n, double hot_frac = 0.2, double hot_prob = 0.8) {
    size_t hot = static_cast<size_t>(static_cast<double>(keys) * hot_frac);
    if (hot < 1) hot = 1;
    std::vector<uint64_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        if (rng.uniform01() < hot_prob) out.push_back(rng.below(hot));
        else out.push_back(hot + rng.below(keys - hot));
    }
    return out;
}

std::vector<uint64_t> gen_temporal(Rng &rng, size_t keys, size_t n, size_t window = 2000) {
    std::vector<uint64_t> out;
    out.reserve(n);
    size_t win_start = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && i % window == 0) win_start = (win_start + window / 4) % keys;
        if (rng.uniform01() < 0.85) out.push_back((win_start + rng.below(std::min(window, keys))) % keys);
        else out.push_back(rng.below(keys));
    }
    return out;
}

struct WorkloadResult {
    std::string name;
    double avg_latency_ns;
    double bandwidth_penalty_ns;
    double dram_baseline_ns;
};

WorkloadResult run_one(const char *name, const std::vector<uint64_t> &keys, size_t addr_base, double dram_latency_ns,
                       int read_bw_gbps, int write_bw_gbps, int read_lat_ns, int write_lat_ns, int capacity_mb,
                       double spacing_ns) {
    CXLMemExpander expander(read_bw_gbps, write_bw_gbps, read_lat_ns, write_lat_ns, /*id=*/0, capacity_mb);

    std::vector<std::tuple<uint64_t, uint64_t>> trace;
    trace.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); i++) {
        trace.emplace_back(static_cast<uint64_t>(static_cast<double>(i) * spacing_ns), addr_base + keys[i]);
    }

    // Drive CXLMemExpander::insert() over the whole trace first. This is
    // what actually makes the model workload-shape-sensitive: insert()
    // classifies a first-ever touch of an address as a store and every
    // repeat touch as a load (see cxlendpoint.cpp), so a skewed workload
    // (zipf/hotspot: many repeat touches of a small hot set) naturally
    // produces a much higher load/store ratio than uniform (mostly
    // first touches across a large key space) -- that ratio is exactly
    // what calculate_bandwidth() reads back out via counter.load/store
    // to weight its congestion model. Calling calculate_bandwidth() and
    // calculate_latency() directly on a trace with pre-seeded occupation
    // and no insert() calls (this file's first draft) skips all of this
    // and produces workload-shape-independent numbers -- verified
    // empirically: all four workloads returned bit-identical output.
    for (const auto &[ts, addr] : trace) {
        expander.insert(ts, /*tid=*/0, addr, addr, /*index=*/0);
    }
    expander.invalidate_cache();

    double latency;
    double bw_penalty;
    {
        // NOTE: CXLMemExpander::calculate_bandwidth() relies on
        // is_address_local() populating address_ranges via
        // update_range_cache() on a cache miss. calculate_latency()
        // instead calls update_address_cache() first, which sets the
        // same cache_valid flag without ever touching address_ranges --
        // if it runs first, every later is_address_local() call in
        // *either* method sees cache_valid==true and skips rebuilding
        // address_ranges, so every address reads as non-local and both
        // calls silently return 0. Calling calculate_bandwidth() first
        // makes the range cache get built correctly; calculate_latency()
        // then reuses it. This is a real ordering bug in CXLMemSim's own
        // cache invalidation, not a misuse on our side -- ordering
        // around it here rather than patching a repo we don't vendor.
        bw_penalty = expander.calculate_bandwidth(trace);
        latency = expander.calculate_latency(trace, dram_latency_ns);
    }

    return {name, latency, bw_penalty, dram_latency_ns};
}

const char *getarg(int argc, char **argv, const char *flag, const char *def) {
    for (int i = 1; i < argc - 1; i++) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return def;
}

} // namespace

int main(int argc, char **argv) {
    size_t keys = static_cast<size_t>(std::atoll(getarg(argc, argv, "--keys", "20000")));
    size_t accesses = static_cast<size_t>(std::atoll(getarg(argc, argv, "--accesses", "200000")));
    uint64_t seed = static_cast<uint64_t>(std::atoll(getarg(argc, argv, "--seed", "20240517")));
    int read_bw = std::atoi(getarg(argc, argv, "--read-bw-gbps", "25"));
    int write_bw = std::atoi(getarg(argc, argv, "--write-bw-gbps", "25"));
    int read_lat = std::atoi(getarg(argc, argv, "--read-lat-ns", "100"));
    int write_lat = std::atoi(getarg(argc, argv, "--write-lat-ns", "150"));
    int capacity_mb = std::atoi(getarg(argc, argv, "--capacity-mb", "256000")); // 256GB, matches server default
    double dram_latency_ns = std::atof(getarg(argc, argv, "--dram-latency-ns", "60"));
    double spacing_ns = std::atof(getarg(argc, argv, "--spacing-ns", "3"));
    const char *out_path = getarg(argc, argv, "--out", nullptr);

    const size_t addr_base = 0x1000;
    std::vector<WorkloadResult> results;
    {
        Rng rng(seed);
        results.push_back(run_one("zipf", gen_zipf(rng, keys, accesses), addr_base, dram_latency_ns, read_bw,
                                   write_bw, read_lat, write_lat, capacity_mb, spacing_ns));
    }
    {
        Rng rng(seed);
        results.push_back(run_one("uniform", gen_uniform(rng, keys, accesses), addr_base, dram_latency_ns, read_bw,
                                   write_bw, read_lat, write_lat, capacity_mb, spacing_ns));
    }
    {
        Rng rng(seed);
        results.push_back(run_one("hotspot", gen_hotspot(rng, keys, accesses), addr_base, dram_latency_ns, read_bw,
                                   write_bw, read_lat, write_lat, capacity_mb, spacing_ns));
    }
    {
        Rng rng(seed);
        results.push_back(run_one("temporal", gen_temporal(rng, keys, accesses), addr_base, dram_latency_ns, read_bw,
                                   write_bw, read_lat, write_lat, capacity_mb, spacing_ns));
    }

    std::string json = "{\n";
    json += "  \"model\": \"cxlmemsim_native\",\n";
    json += "  \"params\": {\"read_bw_gbps\": " + std::to_string(read_bw) +
            ", \"write_bw_gbps\": " + std::to_string(write_bw) + ", \"read_lat_ns\": " + std::to_string(read_lat) +
            ", \"write_lat_ns\": " + std::to_string(write_lat) + ", \"capacity_mb\": " + std::to_string(capacity_mb) +
            ", \"dram_latency_ns\": " + std::to_string(dram_latency_ns) + "},\n";
    json += "  \"workloads\": [\n";
    for (size_t i = 0; i < results.size(); i++) {
        const auto &r = results[i];
        json += "    {\"name\": \"" + r.name + "\", \"avg_latency_ns\": " + std::to_string(r.avg_latency_ns) +
                ", \"bandwidth_penalty_ns\": " + std::to_string(r.bandwidth_penalty_ns) +
                ", \"dram_baseline_ns\": " + std::to_string(r.dram_baseline_ns) + "}";
        json += (i + 1 < results.size()) ? ",\n" : "\n";
    }
    json += "  ]\n}\n";

    if (out_path) {
        FILE *f = std::fopen(out_path, "w");
        if (!f) {
            std::fprintf(stderr, "cannot open %s for writing\n", out_path);
            return 1;
        }
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        std::printf("wrote %s\n", out_path);
    } else {
        std::fwrite(json.data(), 1, json.size(), stdout);
    }
    return 0;
}

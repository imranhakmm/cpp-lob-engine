// Standalone latency/throughput harness comparing OrderBookRef vs OrderBookFast
// on identical workloads.
//
//   * Throughput: median of N runs of the full stream replay (messages/sec).
//   * Latency:    per-message submit latency sampled with steady_clock,
//   reported
//                 as p50 / p99 / p99.9 / max percentile histogram.
//
// Results are printed as a table and written to docs/bench_results.csv for the
// Python plotting layer. Methodology and machine details live in
// docs/BENCHMARKS.md.
//
// Usage: lob_bench_harness [num_messages] [runs] [seed]

#include "lob/event_sink.hpp"
#include "lob/generator.hpp"
#include "lob/order_book_fast.hpp"
#include "lob/order_book_ref.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

struct LatencyStats {
  double p50, p90, p99, p999, max, mean;
};

LatencyStats percentiles(std::vector<double>& ns) {
  std::sort(ns.begin(), ns.end());
  auto pct = [&](double p) {
    if (ns.empty())
      return 0.0;
    std::size_t idx = static_cast<std::size_t>(p * (ns.size() - 1));
    return ns[idx];
  };
  double sum = 0;
  for (double v : ns)
    sum += v;
  return LatencyStats{pct(0.50),  pct(0.90), pct(0.99),
                      pct(0.999), ns.back(), sum / ns.size()};
}

template <class MakeBook>
double throughput_run(const std::vector<Message>& stream, MakeBook make) {
  CountingSink sink;
  auto book = make(sink);
  auto t0 = Clock::now();
  for (const Message& m : stream)
    book.submit(m);
  auto t1 = Clock::now();
  double secs = std::chrono::duration<double>(t1 - t0).count();
  // Touch sink so the optimiser cannot discard the work.
  if (sink.trade_count == 0xFFFFFFFFFFFFFFFFULL)
    std::printf(" ");
  return static_cast<double>(stream.size()) / secs;
}

template <class MakeBook>
double median_throughput(const std::vector<Message>& stream, MakeBook make,
                         int runs) {
  std::vector<double> tputs;
  for (int r = 0; r < runs; ++r)
    tputs.push_back(throughput_run(stream, make));
  std::sort(tputs.begin(), tputs.end());
  return tputs[tputs.size() / 2];
}

template <class MakeBook>
LatencyStats latency_run(const std::vector<Message>& stream, MakeBook make) {
  CountingSink sink;
  auto book = make(sink);
  std::vector<double> samples;
  samples.reserve(stream.size());
  for (const Message& m : stream) {
    auto t0 = Clock::now();
    book.submit(m);
    auto t1 = Clock::now();
    samples.push_back(
        std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  return percentiles(samples);
}

void print_row(const char* name, double tput, const LatencyStats& l) {
  std::printf("%-14s %12.2f  %8.1f %8.1f %8.1f %8.1f %9.1f\n", name, tput / 1e6,
              l.p50, l.p90, l.p99, l.p999, l.max);
}

} // namespace

int main(int argc, char** argv) {
  std::size_t n = argc > 1 ? std::stoul(argv[1]) : 2'000'000;
  int runs = argc > 2 ? std::stoi(argv[2]) : 7;
  std::uint64_t seed = argc > 3 ? std::stoull(argv[3]) : 2024;

  GenConfig cfg;
  cfg.seed = seed;
  SyntheticGenerator gen(cfg);
  std::printf("Generating %zu messages (seed=%llu)...\n", n,
              static_cast<unsigned long long>(seed));
  std::vector<Message> stream = gen.generate(n);
  const Price ticks = cfg.num_ticks;

  auto make_ref = [](CountingSink& s) { return OrderBookRef<CountingSink>(s); };
  auto make_fast = [ticks](CountingSink& s) {
    return OrderBookFast<CountingSink>(s, ticks, 1 << 20);
  };

  // Warm up (cache + branch predictors) before timed runs.
  (void)throughput_run(stream, make_ref);
  (void)throughput_run(stream, make_fast);

  std::printf(
      "\nThroughput: median of %d runs.  Latency: per-message submit.\n", runs);
  std::printf("%-14s %12s  %8s %8s %8s %8s %9s\n", "engine", "Mmsg/s", "p50ns",
              "p90ns", "p99ns", "p99.9ns", "maxns");
  std::printf("----------------------------------------------------------------"
              "----------\n");

  double ref_tput = median_throughput(stream, make_ref, runs);
  LatencyStats ref_lat = latency_run(stream, make_ref);
  print_row("OrderBookRef", ref_tput, ref_lat);

  double fast_tput = median_throughput(stream, make_fast, runs);
  LatencyStats fast_lat = latency_run(stream, make_fast);
  print_row("OrderBookFast", fast_tput, fast_lat);

  std::printf("\nFast/Ref throughput speedup: %.2fx\n", fast_tput / ref_tput);

  // Emit machine-readable results for the plotting layer.
  std::ofstream csv("docs/bench_results.csv");
  if (csv) {
    csv << "engine,throughput_mmsg_s,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,mean_"
           "ns\n";
    auto row = [&](const char* nm, double t, const LatencyStats& l) {
      csv << nm << ',' << (t / 1e6) << ',' << l.p50 << ',' << l.p90 << ','
          << l.p99 << ',' << l.p999 << ',' << l.max << ',' << l.mean << '\n';
    };
    row("OrderBookRef", ref_tput, ref_lat);
    row("OrderBookFast", fast_tput, fast_lat);
    std::printf("Wrote docs/bench_results.csv\n");
  } else {
    std::printf(
        "(could not open docs/bench_results.csv -- run from repo root)\n");
  }
  return 0;
}

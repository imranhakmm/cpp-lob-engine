// Google Benchmark microbenchmarks for the core operations.
//
// We pre-build a workload and a warm book, then measure steady-state cost of:
//   * full mixed-stream replay (per message)
//   * add-then-cancel churn at the top of book
//   * an aggressive order sweeping a level
// for both the reference and optimised engines.

#include "lob/event_sink.hpp"
#include "lob/generator.hpp"
#include "lob/order_book_fast.hpp"
#include "lob/order_book_ref.hpp"

#include <benchmark/benchmark.h>
#include <vector>

using namespace lob;

namespace {

std::vector<Message> make_stream(std::size_t n, std::uint64_t seed = 2024) {
  GenConfig cfg;
  cfg.seed = seed;
  SyntheticGenerator g(cfg);
  return g.generate(n);
}

const Price kTicks = GenConfig{}.num_ticks;

} // namespace

static void BM_Replay_Ref(benchmark::State& state) {
  auto stream = make_stream(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    CountingSink sink;
    OrderBookRef<CountingSink> book(sink);
    for (const auto& m : stream)
      book.submit(m);
    benchmark::DoNotOptimize(sink.trade_count);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Replay_Ref)->Arg(200000);

static void BM_Replay_Fast(benchmark::State& state) {
  auto stream = make_stream(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    CountingSink sink;
    OrderBookFast<CountingSink> book(sink, kTicks, 1 << 20);
    for (const auto& m : stream)
      book.submit(m);
    benchmark::DoNotOptimize(sink.trade_count);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Replay_Fast)->Arg(200000);

// Add/cancel churn: repeatedly post and pull a resting order. Isolates the
// allocation-free add+cancel path of the fast engine vs map/list churn in ref.
static void BM_AddCancel_Ref(benchmark::State& state) {
  CountingSink sink;
  OrderBookRef<CountingSink> book(sink);
  book.submit(Message{MsgType::Limit, 1, Side::Buy, 1000, 100, 0, 0, 0});
  OrderId id = 2;
  for (auto _ : state) {
    book.submit(Message{MsgType::Limit, id, Side::Sell, 2000, 10, 0, 0, 0});
    book.submit(Message{MsgType::Cancel, id, Side::Sell, 0, 0, 0, 0, 0});
    ++id;
  }
}
BENCHMARK(BM_AddCancel_Ref);

static void BM_AddCancel_Fast(benchmark::State& state) {
  CountingSink sink;
  OrderBookFast<CountingSink> book(sink, kTicks, 1 << 16);
  book.submit(Message{MsgType::Limit, 1, Side::Buy, 1000, 100, 0, 0, 0});
  OrderId id = 2;
  for (auto _ : state) {
    book.submit(Message{MsgType::Limit, id, Side::Sell, 2000, 10, 0, 0, 0});
    book.submit(Message{MsgType::Cancel, id, Side::Sell, 0, 0, 0, 0, 0});
    ++id;
  }
}
BENCHMARK(BM_AddCancel_Fast);

BENCHMARK_MAIN();

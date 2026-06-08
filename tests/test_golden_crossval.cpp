// Golden cross-validation: the optimised engine must be observationally
// identical to the reference engine. We replay the same streams through both
// and assert the full trade tape, the full book-update stream, and the final
// book snapshot all match exactly. This is the project's central correctness
// story.

#include "lob/event_sink.hpp"
#include "lob/generator.hpp"
#include "lob/order_book_fast.hpp"
#include "lob/order_book_ref.hpp"
#include "lob/replay.hpp"

#include <gtest/gtest.h>
#include <vector>

using namespace lob;

namespace {

void assert_snapshots_equal(
    const std::vector<OrderBookRef<EventCollector>::LevelView>& a,
    const std::vector<OrderBookFast<EventCollector>::LevelView>& b) {
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].price, b[i].price) << "level " << i;
    EXPECT_EQ(a[i].quantity, b[i].quantity) << "level " << i;
  }
}

void cross_validate(const std::vector<Message>& stream, Price num_ticks) {
  EventCollector ref_ev, fast_ev;
  ref_ev.reserve(stream.size());
  fast_ev.reserve(stream.size());

  OrderBookRef<EventCollector> ref(ref_ev);
  OrderBookFast<EventCollector> fast(fast_ev, num_ticks);

  replay(ref, stream);
  replay(fast, stream);

  ASSERT_EQ(ref_ev.trades.size(), fast_ev.trades.size()) << "trade count";
  EXPECT_EQ(ref_ev.trades, fast_ev.trades) << "trade tape mismatch";

  ASSERT_EQ(ref_ev.updates.size(), fast_ev.updates.size()) << "update count";
  EXPECT_EQ(ref_ev.updates, fast_ev.updates) << "book-update stream mismatch";

  assert_snapshots_equal(ref.snapshot(Side::Buy, -1),
                         fast.snapshot(Side::Buy, -1));
  assert_snapshots_equal(ref.snapshot(Side::Sell, -1),
                         fast.snapshot(Side::Sell, -1));

  EXPECT_EQ(ref.best_bid(), fast.best_bid());
  EXPECT_EQ(ref.best_ask(), fast.best_ask());
}

} // namespace

TEST(Golden, SyntheticDefaultSeed) {
  GenConfig cfg;
  cfg.seed = 1;
  SyntheticGenerator g(cfg);
  cross_validate(g.generate(200000), cfg.num_ticks);
}

TEST(Golden, SyntheticManySeeds) {
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    GenConfig cfg;
    cfg.seed = seed;
    SyntheticGenerator g(cfg);
    cross_validate(g.generate(50000), cfg.num_ticks);
  }
}

TEST(Golden, AggressiveMarketHeavyFlow) {
  GenConfig cfg;
  cfg.seed = 555;
  cfg.p_market = 0.25;
  cfg.p_aggressive = 0.45;
  cfg.half_window = 16; // thin book -> lots of level depletion / best-px moves
  SyntheticGenerator g(cfg);
  cross_validate(g.generate(120000), cfg.num_ticks);
}

TEST(Golden, NarrowTickDomainStress) {
  // Small price domain forces frequent best-price re-scans in the fast engine.
  GenConfig cfg;
  cfg.seed = 909;
  cfg.num_ticks = 256;
  cfg.initial_mid = 128;
  cfg.half_window = 24;
  cfg.p_aggressive = 0.3;
  SyntheticGenerator g(cfg);
  cross_validate(g.generate(80000), cfg.num_ticks);
}

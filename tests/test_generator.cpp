// Generator tests: deterministic reproducibility and basic stream sanity.

#include <gtest/gtest.h>

#include "lob/event_sink.hpp"
#include "lob/generator.hpp"
#include "lob/order_book_ref.hpp"
#include "lob/replay.hpp"

using namespace lob;

TEST(Generator, SameSeedSameStream) {
  GenConfig cfg;
  cfg.seed = 12345;
  SyntheticGenerator a(cfg), b(cfg);
  auto sa = a.generate(50000);
  auto sb = b.generate(50000);
  ASSERT_EQ(sa.size(), sb.size());
  for (std::size_t i = 0; i < sa.size(); ++i) {
    const auto& x = sa[i];
    const auto& y = sb[i];
    ASSERT_EQ(x.type, y.type) << "at " << i;
    ASSERT_EQ(x.id, y.id) << "at " << i;
    ASSERT_EQ(x.side, y.side) << "at " << i;
    ASSERT_EQ(x.price, y.price) << "at " << i;
    ASSERT_EQ(x.quantity, y.quantity) << "at " << i;
    ASSERT_EQ(x.new_price, y.new_price) << "at " << i;
    ASSERT_EQ(x.new_quantity, y.new_quantity) << "at " << i;
  }
}

TEST(Generator, DifferentSeedDiffers) {
  GenConfig a, b;
  a.seed = 1;
  b.seed = 2;
  SyntheticGenerator ga(a), gb(b);
  auto sa = ga.generate(2000);
  auto sb = gb.generate(2000);
  bool any_diff = false;
  for (std::size_t i = 0; i < sa.size(); ++i) {
    if (sa[i].id != sb[i].id || sa[i].price != sb[i].price ||
        sa[i].type != sb[i].type) {
      any_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_diff);
}

TEST(Generator, PricesInRangeAndStreamReplays) {
  GenConfig cfg;
  cfg.seed = 7;
  SyntheticGenerator g(cfg);
  auto stream = g.generate(20000);
  for (const auto& m : stream) {
    if (m.type == MsgType::Limit) {
      EXPECT_GE(m.price, 1);
      EXPECT_LT(m.price, cfg.num_ticks);
    }
  }
  // Replaying through the reference engine must not crash or violate invariants.
  EventCollector ev;
  OrderBookRef<EventCollector> book(ev);
  replay(book, stream);
  if (book.best_bid() && book.best_ask()) {
    EXPECT_LT(*book.best_bid(), *book.best_ask());
  }
  EXPECT_GT(ev.trades.size(), 0u);  // aggressive flow should produce trades
}

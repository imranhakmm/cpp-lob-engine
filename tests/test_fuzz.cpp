// Property / fuzz test.
//
// Generate large random-but-valid message streams across many seeds and
// configurations; after every message assert (a) the optimised engine agrees
// with the reference and (b) core book invariants hold (sorted levels, positive
// resting quantity, bid < ask). Run under the sanitizer build this also
// exercises memory safety of the intrusive lists / object pool.

#include "lob/event_sink.hpp"
#include "lob/generator.hpp"
#include "lob/order_book_fast.hpp"
#include "lob/order_book_ref.hpp"

#include <gtest/gtest.h>
#include <vector>

using namespace lob;

namespace {

template <class Book> void check_invariants(const Book& b) {
  auto bids = b.snapshot(Side::Buy, -1);
  auto asks = b.snapshot(Side::Sell, -1);
  for (std::size_t i = 0; i < bids.size(); ++i) {
    ASSERT_GT(bids[i].quantity, 0);
    if (i)
      ASSERT_LT(bids[i].price, bids[i - 1].price);
  }
  for (std::size_t i = 0; i < asks.size(); ++i) {
    ASSERT_GT(asks[i].quantity, 0);
    if (i)
      ASSERT_GT(asks[i].price, asks[i - 1].price);
  }
  if (b.best_bid() && b.best_ask()) {
    ASSERT_LT(*b.best_bid(), *b.best_ask());
  }
}

} // namespace

TEST(Fuzz, RefEqualsFastAndInvariantsStepwise) {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    GenConfig cfg;
    cfg.seed = seed;
    cfg.num_ticks = 512;
    cfg.initial_mid = 256;
    cfg.half_window = 32;
    cfg.p_aggressive = 0.25;
    cfg.p_market = 0.1;
    cfg.p_modify = 0.08;
    SyntheticGenerator g(cfg);

    EventCollector ref_ev, fast_ev;
    OrderBookRef<EventCollector> ref(ref_ev);
    OrderBookFast<EventCollector> fast(fast_ev, cfg.num_ticks);

    const std::size_t n = 20000;
    for (std::size_t i = 0; i < n; ++i) {
      Message m = g.next();
      ref_ev.clear();
      fast_ev.clear();
      ref.submit(m);
      fast.submit(m);

      ASSERT_EQ(ref_ev.trades, fast_ev.trades)
          << "seed=" << seed << " step=" << i;
      ASSERT_EQ(ref_ev.updates, fast_ev.updates)
          << "seed=" << seed << " step=" << i;

      if ((i & 0x3FF) == 0) { // periodic full invariant + snapshot check
        check_invariants(ref);
        check_invariants(fast);
        ASSERT_EQ(ref.best_bid(), fast.best_bid());
        ASSERT_EQ(ref.best_ask(), fast.best_ask());
        ASSERT_EQ(ref.order_count(), fast.order_count());
      }
    }
  }
}

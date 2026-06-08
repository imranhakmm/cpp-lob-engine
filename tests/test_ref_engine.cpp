// Unit tests for the reference engine: price-time priority, partial fills,
// FIFO within a level, crossing, market sweeps, cancel/modify, edge cases and
// book invariants.

#include <gtest/gtest.h>

#include "lob/event_sink.hpp"
#include "lob/order_book_ref.hpp"

using namespace lob;

namespace {

using Book = OrderBookRef<EventCollector>;

Message limit(OrderId id, Side s, Price px, Quantity q, Timestamp ts) {
  return Message{MsgType::Limit, id, s, px, q, ts, 0, 0};
}
Message market(OrderId id, Side s, Quantity q, Timestamp ts) {
  return Message{MsgType::Market, id, s, 0, q, ts, 0, 0};
}
Message cancel(OrderId id, Timestamp ts) {
  return Message{MsgType::Cancel, id, Side::Buy, 0, 0, ts, 0, 0};
}
Message modify(OrderId id, Price np, Quantity nq, Timestamp ts) {
  return Message{MsgType::Modify, id, Side::Buy, 0, 0, ts, np, nq};
}

}  // namespace

TEST(RefEngine, RestingOrdersNoTrade) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Buy, 100, 10, 1));
  b.submit(limit(2, Side::Sell, 102, 5, 2));
  EXPECT_TRUE(ev.trades.empty());
  EXPECT_EQ(b.best_bid(), 100);
  EXPECT_EQ(b.best_ask(), 102);
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 10);
  EXPECT_EQ(b.qty_at(Side::Sell, 102), 5);
}

TEST(RefEngine, SimpleCross) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Sell, 100, 10, 1));
  b.submit(limit(2, Side::Buy, 100, 4, 2));  // aggressive buy
  ASSERT_EQ(ev.trades.size(), 1u);
  EXPECT_EQ(ev.trades[0].price, 100);
  EXPECT_EQ(ev.trades[0].quantity, 4);
  EXPECT_EQ(ev.trades[0].maker_id, 1u);
  EXPECT_EQ(ev.trades[0].taker_id, 2u);
  EXPECT_EQ(b.qty_at(Side::Sell, 100), 6);  // maker partially filled
  EXPECT_FALSE(b.best_bid().has_value());   // buy fully consumed
}

TEST(RefEngine, FifoWithinLevel) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Buy, 100, 5, 1));  // earlier
  b.submit(limit(2, Side::Buy, 100, 5, 2));  // later
  b.submit(limit(3, Side::Sell, 100, 7, 3));  // sweeps 5 from id1, 2 from id2
  ASSERT_EQ(ev.trades.size(), 2u);
  EXPECT_EQ(ev.trades[0].maker_id, 1u);
  EXPECT_EQ(ev.trades[0].quantity, 5);
  EXPECT_EQ(ev.trades[1].maker_id, 2u);
  EXPECT_EQ(ev.trades[1].quantity, 2);
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 3);  // 3 left on id2
}

TEST(RefEngine, PriceTimePriorityBestFirst) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Sell, 101, 5, 1));
  b.submit(limit(2, Side::Sell, 100, 5, 2));  // better ask
  b.submit(limit(3, Side::Buy, 101, 8, 3));   // crosses both
  ASSERT_EQ(ev.trades.size(), 2u);
  EXPECT_EQ(ev.trades[0].price, 100);  // best price first
  EXPECT_EQ(ev.trades[0].maker_id, 2u);
  EXPECT_EQ(ev.trades[1].price, 101);
  EXPECT_EQ(ev.trades[1].maker_id, 1u);
  EXPECT_EQ(ev.trades[1].quantity, 3);
}

TEST(RefEngine, PartialFillRestsRemainder) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Sell, 100, 4, 1));
  b.submit(limit(2, Side::Buy, 100, 10, 2));  // 4 trade, 6 rest at 100
  ASSERT_EQ(ev.trades.size(), 1u);
  EXPECT_EQ(b.best_bid(), 100);
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 6);
  EXPECT_FALSE(b.best_ask().has_value());
}

TEST(RefEngine, MarketOrderSweepsMultipleLevels) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Sell, 100, 3, 1));
  b.submit(limit(2, Side::Sell, 101, 3, 2));
  b.submit(limit(3, Side::Sell, 102, 3, 3));
  b.submit(market(4, Side::Buy, 7, 4));  // takes 3+3+1
  ASSERT_EQ(ev.trades.size(), 3u);
  EXPECT_EQ(ev.trades[0].price, 100);
  EXPECT_EQ(ev.trades[1].price, 101);
  EXPECT_EQ(ev.trades[2].price, 102);
  EXPECT_EQ(ev.trades[2].quantity, 1);
  EXPECT_EQ(b.qty_at(Side::Sell, 102), 2);
}

TEST(RefEngine, MarketOrderResidualDiscarded) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Sell, 100, 2, 1));
  b.submit(market(2, Side::Buy, 10, 2));  // only 2 available
  ASSERT_EQ(ev.trades.size(), 1u);
  EXPECT_EQ(ev.trades[0].quantity, 2);
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_FALSE(b.best_bid().has_value());  // residual NOT rested
}

TEST(RefEngine, CancelRestingOrder) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Buy, 100, 5, 1));
  b.submit(limit(2, Side::Buy, 100, 5, 2));
  b.submit(cancel(1, 3));
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 5);
  // id2 should still be the resting order
  b.submit(limit(3, Side::Sell, 100, 5, 4));
  ASSERT_EQ(ev.trades.size(), 1u);
  EXPECT_EQ(ev.trades[0].maker_id, 2u);
}

TEST(RefEngine, CancelUnknownIsNoOp) {
  EventCollector ev;
  Book b(ev);
  b.submit(cancel(999, 1));
  EXPECT_TRUE(ev.trades.empty());
  EXPECT_TRUE(ev.updates.empty());
}

TEST(RefEngine, ModifyLosesPriority) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Buy, 100, 5, 1));  // first in queue
  b.submit(limit(2, Side::Buy, 100, 5, 2));
  b.submit(modify(1, 100, 5, 3));  // cancel-replace -> goes to back
  b.submit(limit(3, Side::Sell, 100, 5, 4));
  ASSERT_EQ(ev.trades.size(), 1u);
  EXPECT_EQ(ev.trades[0].maker_id, 2u);  // id2 now ahead of modified id1
}

TEST(RefEngine, ModifyCanCross) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Sell, 100, 5, 1));
  b.submit(limit(2, Side::Buy, 98, 5, 2));
  b.submit(modify(2, 100, 5, 3));  // reprice buy up to 100 -> trades
  ASSERT_EQ(ev.trades.size(), 1u);
  EXPECT_EQ(ev.trades[0].price, 100);
  EXPECT_EQ(ev.trades[0].maker_id, 1u);
}

TEST(RefEngine, EmptyBookQueries) {
  EventCollector ev;
  Book b(ev);
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 0);
  EXPECT_EQ(b.order_count(), 0u);
  b.submit(market(1, Side::Buy, 10, 1));  // market into empty book
  EXPECT_TRUE(ev.trades.empty());
}

TEST(RefEngine, ZeroQuantityRejected) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Buy, 100, 0, 1));
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_TRUE(ev.updates.empty());
}

TEST(RefEngine, SnapshotDepthAndOrdering) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Buy, 100, 1, 1));
  b.submit(limit(2, Side::Buy, 99, 2, 2));
  b.submit(limit(3, Side::Buy, 98, 3, 3));
  auto snap = b.snapshot(Side::Buy, 2);
  ASSERT_EQ(snap.size(), 2u);
  EXPECT_EQ(snap[0].price, 100);  // best first
  EXPECT_EQ(snap[1].price, 99);
}

// Invariant sweep over a small scripted scenario.
TEST(RefEngine, InvariantsHold) {
  EventCollector ev;
  Book b(ev);
  b.submit(limit(1, Side::Buy, 100, 5, 1));
  b.submit(limit(2, Side::Buy, 99, 5, 2));
  b.submit(limit(3, Side::Sell, 103, 5, 3));
  b.submit(limit(4, Side::Sell, 104, 5, 4));
  b.submit(limit(5, Side::Buy, 103, 3, 5));  // partial cross

  auto bids = b.snapshot(Side::Buy, -1);
  auto asks = b.snapshot(Side::Sell, -1);
  // bid prices strictly descending, qty positive
  for (std::size_t i = 0; i < bids.size(); ++i) {
    EXPECT_GT(bids[i].quantity, 0);
    if (i) EXPECT_LT(bids[i].price, bids[i - 1].price);
  }
  for (std::size_t i = 0; i < asks.size(); ++i) {
    EXPECT_GT(asks[i].quantity, 0);
    if (i) EXPECT_GT(asks[i].price, asks[i - 1].price);
  }
  if (b.best_bid() && b.best_ask()) {
    EXPECT_LT(*b.best_bid(), *b.best_ask());
  }
}

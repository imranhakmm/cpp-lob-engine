#pragma once

// OrderBookRef -- the *reference* matching engine.
//
// Deliberately simple and obviously-correct: std::map keyed by price with a
// std::list of orders per level. The map gives sorted levels; the list gives
// FIFO time priority within a level. This is the oracle the optimised engine is
// validated against, so clarity beats speed here.
//
// The matching algorithm and, critically, the *exact order in which events are
// emitted* are documented inline and replicated verbatim by OrderBookFast. That
// shared contract is what makes the golden Ref==Fast test meaningful.

#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

template <class Sink>
class OrderBookRef {
 public:
  struct LevelView {
    Price price;
    Quantity quantity;
  };

  explicit OrderBookRef(Sink& sink) : sink_(sink) {}

  void submit(const Message& m) {
    switch (m.type) {
      case MsgType::Limit:
        add_limit(m.id, m.side, m.price, m.quantity, m.ts);
        break;
      case MsgType::Market:
        add_market(m.id, m.side, m.quantity, m.ts);
        break;
      case MsgType::Cancel:
        cancel(m.id, m.ts);
        break;
      case MsgType::Modify:
        modify(m.id, m.new_price, m.new_quantity, m.ts);
        break;
    }
  }

  void add_limit(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    if (qty <= 0) return;
    Quantity remaining = match(id, side, price, qty, ts, /*is_market=*/false);
    if (remaining > 0) rest(id, side, price, remaining, ts);
  }

  void add_market(OrderId id, Side side, Quantity qty, Timestamp ts) {
    if (qty <= 0) return;
    // Sweep at any price; residual is cancelled (IOC), never rested.
    match(id, side, /*price=*/0, qty, ts, /*is_market=*/true);
  }

  void cancel(OrderId id, Timestamp ts) {
    auto it = index_.find(id);
    if (it == index_.end()) return;
    remove_locator(it->second, ts);
    index_.erase(it);
  }

  void modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts) {
    // Cancel-replace: the order loses time priority and the replacement is run
    // through matching like a fresh limit (it may now cross).
    auto it = index_.find(id);
    if (it == index_.end()) return;
    Side side = it->second.side;
    remove_locator(it->second, ts);
    index_.erase(it);
    if (new_qty > 0) add_limit(id, side, new_price, new_qty, ts);
  }

  std::optional<Price> best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;  // greatest price (descending map)
  }
  std::optional<Price> best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;  // least price (ascending map)
  }

  Quantity qty_at(Side side, Price price) const {
    if (side == Side::Buy) {
      auto it = bids_.find(price);
      return it == bids_.end() ? 0 : it->second.total;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total;
  }

  // Top-`depth` levels for one side, best first.
  std::vector<LevelView> snapshot(Side side, int depth) const {
    std::vector<LevelView> out;
    if (side == Side::Buy) {
      for (const auto& [px, lvl] : bids_) {
        if (depth >= 0 && static_cast<int>(out.size()) >= depth) break;
        out.push_back({px, lvl.total});
      }
    } else {
      for (const auto& [px, lvl] : asks_) {
        if (depth >= 0 && static_cast<int>(out.size()) >= depth) break;
        out.push_back({px, lvl.total});
      }
    }
    return out;
  }

  std::size_t order_count() const { return index_.size(); }

 private:
  struct Level {
    Quantity total{0};
    std::list<Order> orders;  // FIFO: front = oldest = highest priority
  };

  // Greatest-first for bids, least-first for asks.
  using BidMap = std::map<Price, Level, std::greater<Price>>;
  using AskMap = std::map<Price, Level, std::less<Price>>;

  struct Locator {
    Side side;
    Price price;
    std::list<Order>::iterator it;
  };

  // Returns the unfilled remainder of the incoming order.
  Quantity match(OrderId taker_id, Side taker_side, Price limit, Quantity qty,
                 Timestamp ts, bool is_market) {
    if (taker_side == Side::Buy) {
      return match_against(asks_, taker_id, taker_side, limit, qty, ts,
                           is_market, /*buy=*/true);
    }
    return match_against(bids_, taker_id, taker_side, limit, qty, ts, is_market,
                         /*buy=*/false);
  }

  template <class BookSide>
  Quantity match_against(BookSide& opp, OrderId taker_id, Side taker_side,
                         Price limit, Quantity qty, Timestamp ts, bool is_market,
                         bool taker_is_buy) {
    while (qty > 0 && !opp.empty()) {
      auto level_it = opp.begin();  // best opposite level
      Price level_px = level_it->first;
      if (!is_market) {
        bool crosses = taker_is_buy ? (level_px <= limit) : (level_px >= limit);
        if (!crosses) break;
      }
      Level& lvl = level_it->second;
      // Walk the FIFO queue at this level.
      while (qty > 0 && !lvl.orders.empty()) {
        Order& maker = lvl.orders.front();
        Quantity exec = qty < maker.quantity ? qty : maker.quantity;
        sink_.on_trade(Trade{taker_id, maker.id, taker_side, level_px, exec, ts});
        qty -= exec;
        maker.quantity -= exec;
        lvl.total -= exec;
        if (maker.quantity == 0) {
          index_.erase(maker.id);
          lvl.orders.pop_front();
        }
      }
      // One book update per maker level once it is fully processed.
      sink_.on_book_update(
          BookUpdate{opposite(taker_side), level_px, lvl.total, ts});
      if (lvl.total == 0) opp.erase(level_it);
    }
    return qty;
  }

  void rest(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    Level* lvl;
    if (side == Side::Buy) {
      lvl = &bids_[price];
    } else {
      lvl = &asks_[price];
    }
    lvl->orders.push_back(Order{id, side, price, qty, ts});
    lvl->total += qty;
    index_[id] = Locator{side, price, std::prev(lvl->orders.end())};
    sink_.on_book_update(BookUpdate{side, price, lvl->total, ts});
  }

  void remove_locator(const Locator& loc, Timestamp ts) {
    if (loc.side == Side::Buy) {
      auto lit = bids_.find(loc.price);
      Level& lvl = lit->second;
      lvl.total -= loc.it->quantity;
      lvl.orders.erase(loc.it);
      sink_.on_book_update(BookUpdate{Side::Buy, loc.price, lvl.total, ts});
      if (lvl.total == 0) bids_.erase(lit);
    } else {
      auto lit = asks_.find(loc.price);
      Level& lvl = lit->second;
      lvl.total -= loc.it->quantity;
      lvl.orders.erase(loc.it);
      sink_.on_book_update(BookUpdate{Side::Sell, loc.price, lvl.total, ts});
      if (lvl.total == 0) asks_.erase(lit);
    }
  }

  Sink& sink_;
  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Locator> index_;
};

}  // namespace lob

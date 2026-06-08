#pragma once

// OrderBookFast -- the optimised matching engine.
//
// Same public interface and *byte-identical event stream* as OrderBookRef, but
// built for the hot path:
//   * Price levels are a flat array indexed directly by tick -> O(1) lookup,
//     no tree traversal, cache-friendly.
//   * Each level is an intrusive doubly-linked list of order nodes, so cancel
//     from the middle of the queue is O(1).
//   * Order nodes live in a pre-sized object pool with a free list -> no
//     per-message heap allocation on the steady-state path.
//   * Best bid / best ask are cached and only re-scanned when the top level is
//     consumed.
//
// The matching loop mirrors OrderBookRef::match_against line-for-line so the two
// engines emit trades and book updates in exactly the same order. See
// docs/DESIGN.md for the correctness argument.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

template <class Sink>
class OrderBookFast {
 public:
  struct LevelView {
    Price price;
    Quantity quantity;
  };

  // Levels span ticks [0, num_ticks). All message prices must fall in range.
  // pool_capacity pre-reserves order nodes so the steady state never allocates.
  OrderBookFast(Sink& sink, Price num_ticks, std::size_t pool_capacity = 1 << 16)
      : sink_(sink),
        num_ticks_(num_ticks),
        levels_(static_cast<std::size_t>(num_ticks)),
        best_bid_(kNoBid),
        best_ask_(num_ticks),
        bid_lo_(num_ticks),
        ask_hi_(kNoBid) {
    pool_.reserve(pool_capacity);
    index_.reserve(pool_capacity);
  }

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
    Quantity remaining = side == Side::Buy
                             ? match_buy(id, price, qty, ts, false)
                             : match_sell(id, price, qty, ts, false);
    if (remaining > 0) rest(id, side, price, remaining, ts);
  }

  void add_market(OrderId id, Side side, Quantity qty, Timestamp ts) {
    if (qty <= 0) return;
    if (side == Side::Buy) {
      match_buy(id, 0, qty, ts, true);
    } else {
      match_sell(id, 0, qty, ts, true);
    }
  }

  void cancel(OrderId id, Timestamp ts) {
    auto it = index_.find(id);
    if (it == index_.end()) return;
    std::int32_t slot = it->second;
    Node& n = pool_[static_cast<std::size_t>(slot)];
    Side side = n.side;
    Price price = n.price;
    Level& lvl = levels_[static_cast<std::size_t>(price)];
    lvl.total -= n.quantity;
    unlink(lvl, slot);
    free_node(slot);
    index_.erase(it);
    sink_.on_book_update(BookUpdate{side, price, lvl.total, ts});
    if (lvl.total == 0) {
      if (side == Side::Buy && price == best_bid_) retreat_best_bid(price - 1);
      if (side == Side::Sell && price == best_ask_) advance_best_ask(price + 1);
    }
  }

  void modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts) {
    auto it = index_.find(id);
    if (it == index_.end()) return;
    Side side = pool_[static_cast<std::size_t>(it->second)].side;
    cancel(id, ts);
    if (new_qty > 0) add_limit(id, side, new_price, new_qty, ts);
  }

  std::optional<Price> best_bid() const {
    if (best_bid_ == kNoBid) return std::nullopt;
    return best_bid_;
  }
  std::optional<Price> best_ask() const {
    if (best_ask_ == num_ticks_) return std::nullopt;
    return best_ask_;
  }

  Quantity qty_at(Side, Price price) const {
    if (price < 0 || price >= num_ticks_) return 0;
    return levels_[static_cast<std::size_t>(price)].total;
  }

  std::vector<LevelView> snapshot(Side side, int depth) const {
    std::vector<LevelView> out;
    if (side == Side::Buy) {
      for (Price p = best_bid_; p >= bid_lo_; --p) {
        if (depth >= 0 && static_cast<int>(out.size()) >= depth) break;
        const Level& lvl = levels_[static_cast<std::size_t>(p)];
        if (lvl.total > 0) out.push_back({p, lvl.total});
      }
    } else {
      for (Price p = best_ask_; p <= ask_hi_; ++p) {
        if (depth >= 0 && static_cast<int>(out.size()) >= depth) break;
        const Level& lvl = levels_[static_cast<std::size_t>(p)];
        if (lvl.total > 0) out.push_back({p, lvl.total});
      }
    }
    return out;
  }

  std::size_t order_count() const { return index_.size(); }

 private:
  static constexpr Price kNoBid = -1;
  static constexpr std::int32_t kNil = -1;

  struct Node {
    OrderId id;
    Quantity quantity;
    Price price;
    Side side;
    std::int32_t prev;
    std::int32_t next;
  };

  struct Level {
    Quantity total{0};
    std::int32_t head{kNil};  // oldest / highest priority
    std::int32_t tail{kNil};  // newest
  };

  std::int32_t alloc_node(const Node& n) {
    if (!free_list_.empty()) {
      std::int32_t slot = free_list_.back();
      free_list_.pop_back();
      pool_[static_cast<std::size_t>(slot)] = n;
      return slot;
    }
    pool_.push_back(n);
    return static_cast<std::int32_t>(pool_.size() - 1);
  }

  void free_node(std::int32_t slot) { free_list_.push_back(slot); }

  void unlink(Level& lvl, std::int32_t slot) {
    Node& n = pool_[static_cast<std::size_t>(slot)];
    if (n.prev != kNil) {
      pool_[static_cast<std::size_t>(n.prev)].next = n.next;
    } else {
      lvl.head = n.next;
    }
    if (n.next != kNil) {
      pool_[static_cast<std::size_t>(n.next)].prev = n.prev;
    } else {
      lvl.tail = n.prev;
    }
  }

  Quantity match_buy(OrderId taker_id, Price limit, Quantity qty, Timestamp ts,
                     bool is_market) {
    while (qty > 0 && best_ask_ < num_ticks_) {
      Price level_px = best_ask_;
      if (!is_market && level_px > limit) break;
      Level& lvl = levels_[static_cast<std::size_t>(level_px)];
      while (qty > 0 && lvl.head != kNil) {
        Node& maker = pool_[static_cast<std::size_t>(lvl.head)];
        Quantity exec = qty < maker.quantity ? qty : maker.quantity;
        sink_.on_trade(Trade{taker_id, maker.id, Side::Buy, level_px, exec, ts});
        qty -= exec;
        maker.quantity -= exec;
        lvl.total -= exec;
        if (maker.quantity == 0) {
          index_.erase(maker.id);
          std::int32_t done = lvl.head;
          lvl.head = maker.next;
          if (lvl.head != kNil) {
            pool_[static_cast<std::size_t>(lvl.head)].prev = kNil;
          } else {
            lvl.tail = kNil;
          }
          free_node(done);
        }
      }
      sink_.on_book_update(BookUpdate{Side::Sell, level_px, lvl.total, ts});
      if (lvl.total == 0) advance_best_ask(level_px + 1);
    }
    return qty;
  }

  Quantity match_sell(OrderId taker_id, Price limit, Quantity qty, Timestamp ts,
                      bool is_market) {
    while (qty > 0 && best_bid_ >= 0) {
      Price level_px = best_bid_;
      if (!is_market && level_px < limit) break;
      Level& lvl = levels_[static_cast<std::size_t>(level_px)];
      while (qty > 0 && lvl.head != kNil) {
        Node& maker = pool_[static_cast<std::size_t>(lvl.head)];
        Quantity exec = qty < maker.quantity ? qty : maker.quantity;
        sink_.on_trade(Trade{taker_id, maker.id, Side::Sell, level_px, exec, ts});
        qty -= exec;
        maker.quantity -= exec;
        lvl.total -= exec;
        if (maker.quantity == 0) {
          index_.erase(maker.id);
          std::int32_t done = lvl.head;
          lvl.head = maker.next;
          if (lvl.head != kNil) {
            pool_[static_cast<std::size_t>(lvl.head)].prev = kNil;
          } else {
            lvl.tail = kNil;
          }
          free_node(done);
        }
      }
      sink_.on_book_update(BookUpdate{Side::Buy, level_px, lvl.total, ts});
      if (lvl.total == 0) retreat_best_bid(level_px - 1);
    }
    return qty;
  }

  void rest(OrderId id, Side side, Price price, Quantity qty, Timestamp ts) {
    Level& lvl = levels_[static_cast<std::size_t>(price)];
    std::int32_t slot = alloc_node(Node{id, qty, price, side, lvl.tail, kNil});
    if (lvl.tail != kNil) {
      pool_[static_cast<std::size_t>(lvl.tail)].next = slot;
    } else {
      lvl.head = slot;
    }
    lvl.tail = slot;
    lvl.total += qty;
    index_[id] = slot;
    if (side == Side::Buy) {
      if (price > best_bid_) best_bid_ = price;
      if (price < bid_lo_) bid_lo_ = price;
    } else {
      if (price < best_ask_) best_ask_ = price;
      if (price > ask_hi_) ask_hi_ = price;
    }
    sink_.on_book_update(BookUpdate{side, price, lvl.total, ts});
  }

  // Re-scan for the next non-empty ask at or above `from`. The scan is bounded
  // by ask_hi_ (the occupied-ask span), so the common empty-side churn case is
  // O(1) instead of O(tick_domain). ask_hi_ is reset when the side empties.
  void advance_best_ask(Price from) {
    Price p = from;
    while (p <= ask_hi_ && levels_[static_cast<std::size_t>(p)].total == 0) ++p;
    if (p > ask_hi_) {
      best_ask_ = num_ticks_;  // no asks remain
      ask_hi_ = kNoBid;        // reset span so re-population stays tight
    } else {
      best_ask_ = p;
    }
  }

  void retreat_best_bid(Price from) {
    Price p = from;
    while (p >= bid_lo_ && levels_[static_cast<std::size_t>(p)].total == 0) --p;
    if (p < bid_lo_) {
      best_bid_ = kNoBid;   // no bids remain
      bid_lo_ = num_ticks_;  // reset span
    } else {
      best_bid_ = p;
    }
  }

  Sink& sink_;
  Price num_ticks_;
  std::vector<Level> levels_;
  std::vector<Node> pool_;
  std::vector<std::int32_t> free_list_;
  std::unordered_map<OrderId, std::int32_t> index_;
  Price best_bid_;
  Price best_ask_;
  Price bid_lo_;  // lowest occupied bid tick (== num_ticks_ when no bids)
  Price ask_hi_;  // highest occupied ask tick (== kNoBid when no asks)
};

}  // namespace lob

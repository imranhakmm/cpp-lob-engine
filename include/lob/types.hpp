#pragma once

// Core value types for the limit-order-book engine.
//
// Design notes (see docs/DESIGN.md for the full rationale):
//  * Prices are integers measured in *ticks*. No floating point ever enters the
//    matching path, so matching is exact and deterministic across machines.
//  * Quantities are signed 64-bit so accumulated level totals cannot overflow
//  on
//    realistic flows while still letting us assert "never negative" cheaply.
//  * Messages are the single normalized inbound type; both engines consume
//  them.

#include <cstdint>
#include <ostream>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int32_t;    // price in ticks
using Quantity = std::int64_t; // shares/lots
using Timestamp = std::uint64_t;

inline constexpr Price kInvalidPrice = -1;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

enum class MsgType : std::uint8_t {
  Limit = 0,  // add a (possibly aggressive) limit order
  Market = 1, // immediate-or-cancel sweep at any price
  Cancel = 2, // cancel a resting order by id
  Modify = 3, // cancel-replace: new price/qty, loses time priority
};

// A normalized inbound message. The single input type for both engines and the
// replay harness / CSV format.
struct Message {
  MsgType type{MsgType::Limit};
  OrderId id{0};
  Side side{Side::Buy};
  Price price{0};       // ticks; ignored for Market / Cancel
  Quantity quantity{0}; // ignored for Cancel
  Timestamp ts{0};
  Price new_price{0};       // Modify only
  Quantity new_quantity{0}; // Modify only
};

// A resting order as held inside a price level.
struct Order {
  OrderId id{0};
  Side side{Side::Buy};
  Price price{0};
  Quantity quantity{0};
  Timestamp ts{0};
};

// Emitted on every fill. price is always the *maker's* (resting) price.
struct Trade {
  OrderId taker_id{0};
  OrderId maker_id{0};
  Side taker_side{Side::Buy};
  Price price{0};
  Quantity quantity{0};
  Timestamp ts{0};

  friend bool operator==(const Trade& a, const Trade& b) noexcept {
    return a.taker_id == b.taker_id && a.maker_id == b.maker_id &&
           a.taker_side == b.taker_side && a.price == b.price &&
           a.quantity == b.quantity && a.ts == b.ts;
  }
};

// Emitted whenever the total resting quantity at a price level changes.
// quantity is the *new* aggregate resting size at that level (0 => removed).
struct BookUpdate {
  Side side{Side::Buy};
  Price price{0};
  Quantity quantity{0};
  Timestamp ts{0};

  friend bool operator==(const BookUpdate& a, const BookUpdate& b) noexcept {
    return a.side == b.side && a.price == b.price && a.quantity == b.quantity &&
           a.ts == b.ts;
  }
};

inline const char* to_string(Side s) noexcept {
  return s == Side::Buy ? "BUY" : "SELL";
}

inline std::ostream& operator<<(std::ostream& os, const Trade& t) {
  return os << "Trade{taker=" << t.taker_id << ",maker=" << t.maker_id
            << ",side=" << to_string(t.taker_side) << ",px=" << t.price
            << ",qty=" << t.quantity << ",ts=" << t.ts << "}";
}

inline std::ostream& operator<<(std::ostream& os, const BookUpdate& u) {
  return os << "BookUpdate{side=" << to_string(u.side) << ",px=" << u.price
            << ",qty=" << u.quantity << ",ts=" << u.ts << "}";
}

} // namespace lob

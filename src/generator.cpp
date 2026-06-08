#include "lob/generator.hpp"

namespace lob {

Message SyntheticGenerator::next() {
  ++ts_;

  // Mid performs a lazy random walk so the book drifts over time.
  if (rand_unit() < cfg_.p_mid_move) {
    mid_ = clamp_price(mid_ + (rand_unit() < 0.5 ? -1 : 1));
  }

  const double u = rand_unit();
  const bool have_live = !live_.empty();

  // Cancel a resting order.
  if (have_live && u < cfg_.p_cancel) {
    std::size_t idx = static_cast<std::size_t>(
        rand_int(0, static_cast<std::int64_t>(live_.size()) - 1));
    OrderId victim = live_[idx];
    live_[idx] = live_.back();
    live_.pop_back();
    return Message{MsgType::Cancel, victim, Side::Buy, 0, 0, ts_, 0, 0};
  }

  // Market sweep.
  if (u < cfg_.p_cancel + cfg_.p_market) {
    Side side = rand_unit() < 0.5 ? Side::Buy : Side::Sell;
    Quantity qty = rand_int(1, cfg_.max_size);
    return Message{MsgType::Market, next_id_++, side, 0, qty, ts_, 0, 0};
  }

  // Modify an existing resting order (cancel-replace).
  if (have_live && u < cfg_.p_cancel + cfg_.p_market + cfg_.p_modify) {
    std::size_t idx = static_cast<std::size_t>(
        rand_int(0, static_cast<std::int64_t>(live_.size()) - 1));
    OrderId target = live_[idx];
    Side side = rand_unit() < 0.5 ? Side::Buy : Side::Sell;
    Price offset = static_cast<Price>(rand_int(0, cfg_.half_window));
    Price new_px = clamp_price(side == Side::Buy ? mid_ - offset : mid_ + offset);
    Quantity new_qty = rand_int(1, cfg_.max_size);
    return Message{MsgType::Modify, target, side, 0, 0, ts_, new_px, new_qty};
  }

  // Otherwise: a fresh limit order.
  Side side = rand_unit() < 0.5 ? Side::Buy : Side::Sell;
  Quantity qty = rand_int(1, cfg_.max_size);
  Price px;
  if (rand_unit() < cfg_.p_aggressive) {
    // Aggressive: priced through the mid so it is likely to trade.
    Price thru = static_cast<Price>(rand_int(0, 3));
    px = clamp_price(side == Side::Buy ? mid_ + thru : mid_ - thru);
  } else {
    Price depth = static_cast<Price>(rand_int(0, cfg_.half_window));
    px = clamp_price(side == Side::Buy ? mid_ - depth : mid_ + depth);
  }
  OrderId id = next_id_++;
  live_.push_back(id);
  return Message{MsgType::Limit, id, side, px, qty, ts_, 0, 0};
}

}  // namespace lob

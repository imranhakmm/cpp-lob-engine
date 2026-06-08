#pragma once

// Event sinks decouple the matching core from any I/O. Engines are templated on
// the sink type so the hot path makes *direct, non-virtual* calls -- zero
// dispatch overhead -- while tests, replay and bindings plug in whatever sink
// they need.
//
// A Sink must provide:
//     void on_trade(const Trade&);
//     void on_book_update(const BookUpdate&);

#include "lob/types.hpp"

#include <vector>

namespace lob {

// Discards everything. Used to benchmark pure matching cost.
struct NullSink {
  void on_trade(const Trade&) noexcept {}
  void on_book_update(const BookUpdate&) noexcept {}
};

// Records the full trade tape and book-update stream. Used by the replay
// harness and, crucially, by the Ref==Fast golden cross-validation test.
struct EventCollector {
  std::vector<Trade> trades;
  std::vector<BookUpdate> updates;

  void on_trade(const Trade& t) { trades.push_back(t); }
  void on_book_update(const BookUpdate& u) { updates.push_back(u); }

  void clear() {
    trades.clear();
    updates.clear();
  }

  void reserve(std::size_t n) {
    trades.reserve(n);
    updates.reserve(n);
  }
};

// Counts events only -- cheap sink for throughput benchmarks where we still
// want to defeat dead-code elimination.
struct CountingSink {
  std::uint64_t trade_count{0};
  std::uint64_t update_count{0};
  Quantity volume{0};

  void on_trade(const Trade& t) noexcept {
    ++trade_count;
    volume += t.quantity;
  }
  void on_book_update(const BookUpdate&) noexcept { ++update_count; }
};

} // namespace lob

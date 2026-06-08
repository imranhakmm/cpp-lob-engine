#pragma once

// Seedable synthetic order-flow generator.
//
// Produces a reproducible stream of normalized Messages with Poisson-like
// arrivals of limits / cancels / markets / modifies, sizes from a heavy-ish
// distribution, and prices scattered around a randomly-walking mid. "Same seed
// => identical stream" is guaranteed by driving everything off a single
// std::mt19937_64 and sampling with our own integer helpers (so we do not
// depend on the platform's distribution implementations).

#include "lob/types.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace lob {

struct GenConfig {
  std::uint64_t seed = 42;
  Price num_ticks = 200000;   // price domain [0, num_ticks)
  Price initial_mid = 100000; // starting mid in ticks
  Price half_window = 64;     // typical resting depth on each side of mid
  double p_cancel = 0.20;     // action mix (limit gets the remainder)
  double p_market = 0.06;
  double p_modify = 0.04;
  double p_aggressive = 0.18; // chance a fresh limit crosses the spread
  double p_mid_move = 0.05;   // chance the mid takes a +/-1 step per message
  Quantity max_size = 50;
};

class SyntheticGenerator {
public:
  explicit SyntheticGenerator(GenConfig cfg)
      : cfg_(cfg), rng_(cfg.seed), mid_(cfg.initial_mid) {
    live_.reserve(1 << 16);
  }

  // Produce the next message in the stream.
  Message next();

  // Convenience: generate exactly n messages.
  std::vector<Message> generate(std::size_t n) {
    std::vector<Message> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
      out.push_back(next());
    return out;
  }

private:
  // Uniform integer in [lo, hi] from the raw 64-bit stream (deterministic
  // across standard-library implementations).
  std::int64_t rand_int(std::int64_t lo, std::int64_t hi) {
    std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1ULL;
    return lo + static_cast<std::int64_t>(rng_() % span);
  }
  double rand_unit() {
    // 53-bit mantissa worth of randomness in [0, 1).
    return (rng_() >> 11) * (1.0 / 9007199254740992.0);
  }

  Price clamp_price(std::int64_t p) const {
    if (p < 1)
      return 1;
    if (p > cfg_.num_ticks - 2)
      return cfg_.num_ticks - 2;
    return static_cast<Price>(p);
  }

  GenConfig cfg_;
  std::mt19937_64 rng_;
  Price mid_;
  Timestamp ts_{0};
  OrderId next_id_{1};
  std::vector<OrderId> live_; // ids we have added and not yet cancelled
};

} // namespace lob

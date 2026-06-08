// CSV format tests: round-trip fidelity and replay equivalence.

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>

#include "lob/event_sink.hpp"
#include "lob/generator.hpp"
#include "lob/order_book_ref.hpp"
#include "lob/replay.hpp"

using namespace lob;

namespace {
bool msg_eq(const Message& a, const Message& b) {
  return a.type == b.type && a.id == b.id && a.side == b.side &&
         a.price == b.price && a.quantity == b.quantity &&
         a.new_price == b.new_price && a.new_quantity == b.new_quantity &&
         a.ts == b.ts;
}

// Unique temp path without the deprecated std::tmpnam.
std::string temp_csv_path() {
  static std::atomic<unsigned> counter{0};
  auto dir = std::filesystem::temp_directory_path();
  auto name = "lob_csv_" + std::to_string(counter.fetch_add(1)) + "_" +
              std::to_string(::getpid()) + ".csv";
  return (dir / name).string();
}
}  // namespace

TEST(Csv, LineRoundTrip) {
  Message m{MsgType::Modify, 42, Side::Sell, 0, 0, 99, 12345, 7};
  std::string line = to_csv_line(m);
  Message back = parse_csv_line(line);
  EXPECT_TRUE(msg_eq(m, back)) << line;
}

TEST(Csv, FileRoundTrip) {
  GenConfig cfg;
  cfg.seed = 99;
  SyntheticGenerator g(cfg);
  auto original = g.generate(5000);

  std::string path = temp_csv_path();
  write_csv(path, original);
  auto loaded = load_csv(path);
  std::remove(path.c_str());

  ASSERT_EQ(original.size(), loaded.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_TRUE(msg_eq(original[i], loaded[i])) << "mismatch at " << i;
  }
}

TEST(Csv, ReplayEquivalentAfterRoundTrip) {
  GenConfig cfg;
  cfg.seed = 314;
  SyntheticGenerator g(cfg);
  auto original = g.generate(8000);

  std::string path = temp_csv_path();
  write_csv(path, original);
  auto loaded = load_csv(path);
  std::remove(path.c_str());

  EventCollector ev1, ev2;
  OrderBookRef<EventCollector> b1(ev1), b2(ev2);
  replay(b1, original);
  replay(b2, loaded);
  ASSERT_EQ(ev1.trades.size(), ev2.trades.size());
  EXPECT_EQ(ev1.trades, ev2.trades);
  EXPECT_EQ(ev1.updates, ev2.updates);
}

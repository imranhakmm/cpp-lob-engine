// pybind11 module "lobpy": drive the C++ matching engine from Python and pull
// the trade tape, per-message top-of-book and final snapshot back as numpy
// arrays for the analytics layer.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/event_sink.hpp"
#include "lob/generator.hpp"
#include "lob/order_book_fast.hpp"
#include "lob/replay.hpp"

namespace py = pybind11;
using namespace lob;

namespace {

template <class T>
py::array_t<T> to_np(const std::vector<T>& v) {
  py::array_t<T> a(static_cast<py::ssize_t>(v.size()));
  if (!v.empty()) std::memcpy(a.mutable_data(), v.data(), v.size() * sizeof(T));
  return a;
}

// Columnar capture of a replay: the message stream, top-of-book sampled after
// every message, and the full trade tape. Everything is index/timestamp aligned
// so Python can build microstructure features directly.
struct SimColumns {
  // L1 after each message (length = #messages)
  std::vector<std::int64_t> ts;
  std::vector<std::int32_t> best_bid, best_ask;
  std::vector<std::int64_t> bid_size, ask_size;
  // trade tape
  std::vector<std::int64_t> tr_ts;
  std::vector<std::int32_t> tr_price;
  std::vector<std::int64_t> tr_qty;
  std::vector<std::int8_t> tr_taker_side;  // 0=Buy,1=Sell
  std::vector<std::uint64_t> tr_taker_id, tr_maker_id;
};

void record_l1(SimColumns& c, OrderBookFast<EventCollector>& book, Timestamp ts) {
  c.ts.push_back(static_cast<std::int64_t>(ts));
  auto bb = book.best_bid();
  auto ba = book.best_ask();
  c.best_bid.push_back(bb ? *bb : kInvalidPrice);
  c.best_ask.push_back(ba ? *ba : kInvalidPrice);
  c.bid_size.push_back(bb ? book.qty_at(Side::Buy, *bb) : 0);
  c.ask_size.push_back(ba ? book.qty_at(Side::Sell, *ba) : 0);
}

SimColumns run(const std::vector<Message>& stream, Price num_ticks) {
  EventCollector ev;
  ev.reserve(stream.size());
  OrderBookFast<EventCollector> book(ev, num_ticks, 1 << 20);
  SimColumns c;
  c.ts.reserve(stream.size());
  for (const Message& m : stream) {
    book.submit(m);
    record_l1(c, book, m.ts);
  }
  for (const Trade& t : ev.trades) {
    c.tr_ts.push_back(static_cast<std::int64_t>(t.ts));
    c.tr_price.push_back(t.price);
    c.tr_qty.push_back(t.quantity);
    c.tr_taker_side.push_back(t.taker_side == Side::Buy ? 0 : 1);
    c.tr_taker_id.push_back(t.taker_id);
    c.tr_maker_id.push_back(t.maker_id);
  }
  return c;
}

py::dict columns_to_dict(const SimColumns& c) {
  py::dict l1;
  l1["ts"] = to_np(c.ts);
  l1["best_bid"] = to_np(c.best_bid);
  l1["best_ask"] = to_np(c.best_ask);
  l1["bid_size"] = to_np(c.bid_size);
  l1["ask_size"] = to_np(c.ask_size);

  py::dict trades;
  trades["ts"] = to_np(c.tr_ts);
  trades["price"] = to_np(c.tr_price);
  trades["qty"] = to_np(c.tr_qty);
  trades["taker_side"] = to_np(c.tr_taker_side);
  trades["taker_id"] = to_np(c.tr_taker_id);
  trades["maker_id"] = to_np(c.tr_maker_id);

  py::dict out;
  out["l1"] = l1;
  out["trades"] = trades;
  return out;
}

py::dict simulate(std::size_t n, std::uint64_t seed, Price num_ticks,
                  Price initial_mid, double p_market, double p_aggressive) {
  GenConfig cfg;
  cfg.seed = seed;
  cfg.num_ticks = num_ticks;
  cfg.initial_mid = initial_mid;
  cfg.p_market = p_market;
  cfg.p_aggressive = p_aggressive;
  SyntheticGenerator g(cfg);
  return columns_to_dict(run(g.generate(n), num_ticks));
}

py::dict replay_csv(const std::string& path, Price num_ticks) {
  return columns_to_dict(run(load_csv(path), num_ticks));
}

}  // namespace

// Thin interactive wrapper around the optimised engine.
class Book {
 public:
  explicit Book(Price num_ticks = 200000, std::size_t pool = 1 << 16)
      : book_(ev_, num_ticks, pool) {}

  void limit(OrderId id, int side, Price px, Quantity qty, Timestamp ts) {
    book_.add_limit(id, side ? Side::Sell : Side::Buy, px, qty, ts);
  }
  void market(OrderId id, int side, Quantity qty, Timestamp ts) {
    book_.add_market(id, side ? Side::Sell : Side::Buy, qty, ts);
  }
  void cancel(OrderId id, Timestamp ts) { book_.cancel(id, ts); }
  void modify(OrderId id, Price np, Quantity nq, Timestamp ts) {
    book_.modify(id, np, nq, ts);
  }

  py::object best_bid() const {
    auto b = book_.best_bid();
    return b ? py::cast(*b) : py::none();
  }
  py::object best_ask() const {
    auto a = book_.best_ask();
    return a ? py::cast(*a) : py::none();
  }
  Quantity qty_at(int side, Price px) const {
    return book_.qty_at(side ? Side::Sell : Side::Buy, px);
  }

  py::tuple snapshot(int side, int depth) const {
    auto levels = book_.snapshot(side ? Side::Sell : Side::Buy, depth);
    std::vector<std::int32_t> px;
    std::vector<std::int64_t> qty;
    for (const auto& l : levels) {
      px.push_back(l.price);
      qty.push_back(l.quantity);
    }
    return py::make_tuple(to_np(px), to_np(qty));
  }

  std::size_t num_trades() const { return ev_.trades.size(); }
  void clear_events() { ev_.clear(); }

 private:
  EventCollector ev_;
  OrderBookFast<EventCollector> book_;
};

PYBIND11_MODULE(lobpy, m) {
  m.doc() = "High-performance limit-order-book engine (C++ core via pybind11)";

  py::class_<Book>(m, "Book")
      .def(py::init<Price, std::size_t>(), py::arg("num_ticks") = 200000,
           py::arg("pool") = (1 << 16))
      .def("limit", &Book::limit, py::arg("id"), py::arg("side"),
           py::arg("price"), py::arg("qty"), py::arg("ts") = 0,
           "side: 0=buy, 1=sell")
      .def("market", &Book::market, py::arg("id"), py::arg("side"),
           py::arg("qty"), py::arg("ts") = 0)
      .def("cancel", &Book::cancel, py::arg("id"), py::arg("ts") = 0)
      .def("modify", &Book::modify, py::arg("id"), py::arg("new_price"),
           py::arg("new_qty"), py::arg("ts") = 0)
      .def("best_bid", &Book::best_bid)
      .def("best_ask", &Book::best_ask)
      .def("qty_at", &Book::qty_at, py::arg("side"), py::arg("price"))
      .def("snapshot", &Book::snapshot, py::arg("side"), py::arg("depth") = -1,
           "Returns (prices, quantities) numpy arrays, best level first.")
      .def("num_trades", &Book::num_trades)
      .def("clear_events", &Book::clear_events);

  m.def("simulate", &simulate, py::arg("n"), py::arg("seed") = 2024,
        py::arg("num_ticks") = 200000, py::arg("initial_mid") = 100000,
        py::arg("p_market") = 0.06, py::arg("p_aggressive") = 0.18,
        "Generate synthetic flow, replay through the optimised engine and "
        "return {'l1': {...}, 'trades': {...}} as numpy column arrays.");

  m.def("replay_csv", &replay_csv, py::arg("path"), py::arg("num_ticks") = 200000,
        "Replay a normalized CSV message file; same return shape as simulate().");
}

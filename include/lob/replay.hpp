#pragma once

// Replay harness + normalized CSV message format.
//
// CSV columns (one message per row, header required):
//     ts,type,id,side,price,qty,new_price,new_qty
//   type : L=Limit  M=Market  C=Cancel  X=Modify
//   side : B=Buy    S=Sell
// Unused fields for a given type are written as 0. Market/Cancel ignore price.
//
// This is the seam for external order-by-order data: convert any feed (incl.
// LOBSTER message files, see data/README.md) into this schema and replay it
// through either engine.

#include "lob/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lob {

// Feed a message stream into any engine exposing submit(const Message&).
template <class Engine>
void replay(Engine& engine, const std::vector<Message>& messages) {
  for (const Message& m : messages)
    engine.submit(m);
}

// --- CSV serialization (implemented in src/replay.cpp) ---------------------

std::string to_csv_line(const Message& m);
Message parse_csv_line(const std::string& line); // throws std::runtime_error

void write_csv(const std::string& path, const std::vector<Message>& messages);
std::vector<Message> load_csv(const std::string& path); // skips header

} // namespace lob

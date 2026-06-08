#include "lob/replay.hpp"

#include <array>
#include <charconv>
#include <fstream>
#include <stdexcept>

namespace lob {
namespace {

char type_char(MsgType t) {
  switch (t) {
    case MsgType::Limit:
      return 'L';
    case MsgType::Market:
      return 'M';
    case MsgType::Cancel:
      return 'C';
    case MsgType::Modify:
      return 'X';
  }
  return '?';
}

MsgType parse_type(char c) {
  switch (c) {
    case 'L':
      return MsgType::Limit;
    case 'M':
      return MsgType::Market;
    case 'C':
      return MsgType::Cancel;
    case 'X':
      return MsgType::Modify;
    default:
      throw std::runtime_error(std::string("bad message type: ") + c);
  }
}

std::int64_t to_i64(const std::string& s) {
  std::int64_t v = 0;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, v);
  if (ec != std::errc{} || ptr != last) {
    throw std::runtime_error("bad integer field: '" + s + "'");
  }
  return v;
}

}  // namespace

std::string to_csv_line(const Message& m) {
  std::string out;
  out.reserve(48);
  out += std::to_string(m.ts);
  out += ',';
  out += type_char(m.type);
  out += ',';
  out += std::to_string(m.id);
  out += ',';
  out += (m.side == Side::Buy ? 'B' : 'S');
  out += ',';
  out += std::to_string(m.price);
  out += ',';
  out += std::to_string(m.quantity);
  out += ',';
  out += std::to_string(m.new_price);
  out += ',';
  out += std::to_string(m.new_quantity);
  return out;
}

Message parse_csv_line(const std::string& line) {
  std::array<std::string, 8> fields;
  std::size_t fi = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == ',') {
      if (fi >= fields.size()) {
        throw std::runtime_error("too many fields in line: " + line);
      }
      fields[fi++] = line.substr(start, i - start);
      start = i + 1;
    }
  }
  if (fi != 8) {
    throw std::runtime_error("expected 8 fields, got " + std::to_string(fi) +
                             " in line: " + line);
  }
  Message m;
  m.ts = static_cast<Timestamp>(to_i64(fields[0]));
  m.type = parse_type(fields[1].empty() ? '?' : fields[1][0]);
  m.id = static_cast<OrderId>(to_i64(fields[2]));
  m.side = (!fields[3].empty() && fields[3][0] == 'S') ? Side::Sell : Side::Buy;
  m.price = static_cast<Price>(to_i64(fields[4]));
  m.quantity = static_cast<Quantity>(to_i64(fields[5]));
  m.new_price = static_cast<Price>(to_i64(fields[6]));
  m.new_quantity = static_cast<Quantity>(to_i64(fields[7]));
  return m;
}

void write_csv(const std::string& path, const std::vector<Message>& messages) {
  std::ofstream os(path);
  if (!os) throw std::runtime_error("cannot open for write: " + path);
  os << "ts,type,id,side,price,qty,new_price,new_qty\n";
  for (const Message& m : messages) os << to_csv_line(m) << '\n';
}

std::vector<Message> load_csv(const std::string& path) {
  std::ifstream is(path);
  if (!is) throw std::runtime_error("cannot open for read: " + path);
  std::vector<Message> out;
  std::string line;
  bool first = true;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (first) {
      first = false;
      if (line.rfind("ts,", 0) == 0) continue;  // skip header
    }
    out.push_back(parse_csv_line(line));
  }
  return out;
}

}  // namespace lob

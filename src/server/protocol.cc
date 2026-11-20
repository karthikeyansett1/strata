#include "protocol.h"

#include <charconv>

namespace strata::server {
namespace {

bool Text(std::string_view value) {
  return value.find_first_of(std::string_view("\0\r\n", 3)) == std::string_view::npos;
}

bool Key(std::string_view value) {
  return !value.empty() && Text(value) && value.find_first_of(" \t") == std::string_view::npos;
}

std::pair<std::string_view, std::string_view> Split(std::string_view value) {
  const auto space = value.find(' ');
  if (space == std::string_view::npos) {
    return {value, {}};
  }
  return {value.substr(0, space), value.substr(space + 1)};
}

template <class Number>
bool Parse(std::string_view text, Number* value) {
  if (text.empty()) {
    return false;
  }
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool ParseVector(std::string_view text, std::vector<float>* values) {
  while (true) {
    const auto comma = text.find(',');
    float value;
    if (values->size() >= 4096 || !Parse(text.substr(0, comma), &value)) {
      return false;
    }
    values->push_back(value);
    if (comma == std::string_view::npos) {
      return true;
    }
    text.remove_prefix(comma + 1);
  }
}

std::string Reply(const Status& status) {
  switch (status.code()) {
    case StatusCode::kOk:
      return "OK\n";
    case StatusCode::kNotFound:
      return "NOT_FOUND\n";
    case StatusCode::kCorruption:
      return "ERR storage corruption\n";
    case StatusCode::kIOError:
      return "ERR storage I/O failure\n";
    case StatusCode::kInvalidArgument:
      return "ERR invalid argument\n";
    case StatusCode::kBusy:
      return "ERR storage busy\n";
  }
  return "ERR storage failure\n";
}

}  // namespace

bool Input::Append(std::string_view data) {
  buffer_.append(data);
  const auto newline = buffer_.find('\n');
  return (newline == std::string::npos ? buffer_.size() : newline) <= kMaxLine;
}

std::optional<std::string> Input::Next() {
  const auto newline = buffer_.find('\n');
  if (newline == std::string::npos) {
    return std::nullopt;
  }
  auto line = buffer_.substr(0, newline);
  buffer_.erase(0, newline + 1);
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

std::string Execute(DB& db, std::string_view line, VectorStore* vectors) {
  if (line.size() > kMaxLine || !Text(line)) {
    return "ERR malformed command\n";
  }
  const auto [command, args] = Split(line);
  if (command == "PUT") {
    const auto [key, value] = Split(args);
    if (Key(key) && args.find(' ') != std::string_view::npos) {
      return Reply(db.Put(key, value));
    }
  } else if (command == "GET" && Key(args)) {
    std::string value;
    const auto status = db.Get(args, &value);
    if (!status.ok()) {
      return Reply(status);
    }
    return Text(value) && value.size() <= kMaxResponse - 7 ? "VALUE " + value + "\n"
                                                           : "ERR value cannot be encoded\n";
  } else if (command == "DEL" && Key(args)) {
    return Reply(db.Delete(args));
  } else if (command == "SCAN") {
    const auto [start, end] = Split(args);
    if (Key(start) && Key(end)) {
      std::vector<std::pair<std::string, std::string>> rows;
      const auto status = db.Scan(start, end, &rows);
      if (!status.ok()) {
        return Reply(status);
      }
      std::string response;
      for (const auto& [key, value] : rows) {
        if (!Key(key) || !Text(value) ||
            key.size() + value.size() + 6 > kMaxResponse - response.size()) {
          return "ERR scan response cannot be encoded\n";
        }
        response.append(key).append(" ").append(value).append("\n");
      }
      return response + "END\n";
    }
  } else if (vectors && (command == "VPUT" || command == "VSEARCH")) {
    const auto [argument, coordinates] = Split(args);
    std::vector<float> values;
    if (!ParseVector(coordinates, &values)) {
      return "ERR invalid vector\n";
    }
    if (command == "VPUT" && Key(argument)) {
      return Reply(vectors->Put(argument, values));
    }
    std::size_t k = 0;
    if (command == "VSEARCH" && Parse(argument, &k) && k > 0 && k <= 1000) {
      std::vector<VectorHit> hits;
      if (auto status = vectors->Search(values, k, &hits); !status.ok()) {
        return Reply(status);
      }
      std::string response;
      for (const auto& hit : hits) {
        if (!Key(hit.id)) {
          return "ERR vector id cannot be encoded\n";
        }
        response.append(hit.id).append(" ").append(std::to_string(hit.distance)).append("\n");
      }
      return response + "END\n";
    }
  } else if (line == "STATS") {
    const auto stats = db.GetStats();
    return "seq=" + std::to_string(stats.sequence) +
           "\nwal_syncs=" + std::to_string(stats.wal_syncs) +
           "\nsstable_count=" + std::to_string(stats.sstable_count) +
           "\ncompactions=" + std::to_string(stats.compactions) + "\nEND\n";
  }
  return "ERR malformed or unknown command\n";
}

}  // namespace strata::server

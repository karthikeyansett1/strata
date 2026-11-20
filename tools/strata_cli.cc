#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "strata/db.h"

int main(int argc, char** argv) {
  const std::string_view command = argc > 2 ? argv[2] : "";
  if (!((argc == 5 && command == "put") || (argc == 4 && (command == "get" || command == "del")))) {
    std::cerr << "Usage: strata_cli <db-path> put <key> <value>\n"
                 "       strata_cli <db-path> get <key>\n"
                 "       strata_cli <db-path> del <key>\n";
    return 2;
  }

  std::unique_ptr<strata::DB> db;
  auto status = strata::DB::Open({}, argv[1], &db);
  if (status.ok()) {
    if (command == "put") {
      status = db->Put(argv[3], argv[4]);
    } else if (command == "del") {
      status = db->Delete(argv[3]);
    } else {
      std::string value;
      status = db->Get(argv[3], &value);
      if (status.ok()) {
        std::cout << value << '\n';
      }
    }
  }
  if (status.code() == strata::StatusCode::kNotFound) {
    std::cerr << "NOT_FOUND\n";
    return 3;
  }
  if (!status.ok()) {
    std::cerr << "strata_cli: "
              << (status.message().empty() ? "operation failed" : status.message()) << '\n';
    return 1;
  }
  return 0;
}

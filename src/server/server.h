#pragma once

#include <cstdint>
#include <functional>
#include <stop_token>

#include "strata/db.h"

namespace strata::server {

Status Serve(DB& db, std::uint16_t port, std::size_t threads, std::stop_token stop,
             const std::function<void(std::uint16_t)>& ready);

}  // namespace strata::server

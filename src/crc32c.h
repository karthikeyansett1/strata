#pragma once

#include <cstdint>
#include <string_view>

namespace strata {

std::uint32_t Crc32c(std::string_view bytes) noexcept;

}  // namespace strata

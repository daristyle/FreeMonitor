#pragma once

#include "Error.hpp"

#include <cstdint>
#include <vector>

namespace fm {
    Result<std::vector<std::uint32_t>> ipv4BroadcastAddresses();
}

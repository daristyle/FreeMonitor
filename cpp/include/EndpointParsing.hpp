#pragma once

#include "UDPSocket.hpp"

#include <optional>
#include <string_view>

namespace fm {
    std::optional<Endpoint> parseEndpoint(std::string_view text);
}

#pragma once

#include "UDPSocket.hpp"

#include <cstdint>

namespace fm {
    struct Session {
        Endpoint peer;
        std::uint32_t sessionId = 0;
        std::uint32_t nextSeq = 0;
        std::uint8_t features = 0;
    };
}

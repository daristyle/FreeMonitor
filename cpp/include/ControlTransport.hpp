#pragma once

#include "UDPSocket.hpp"

#include <cstdint>
#include <span>

namespace fm {
    class ControlTransport {
    public:
        virtual ~ControlTransport() = default;

        virtual void send(std::span<const std::uint8_t> packet, const Endpoint& to) noexcept = 0;
    };
}

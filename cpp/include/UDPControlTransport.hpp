#pragma once

#include "ControlTransport.hpp"
#include "UDPSocket.hpp"

#include <cstdint>
#include <span>

namespace fm {
    class UDPControlTransport final : public ControlTransport {
    private:
        UDPSocket& udpSocket;

    public:
        explicit UDPControlTransport(UDPSocket& udpSocket);

        void send(std::span<const std::uint8_t> packet, const Endpoint& to) noexcept override;
    };
}

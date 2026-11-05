#include "UDPControlTransport.hpp"

namespace fm {
    UDPControlTransport::UDPControlTransport(UDPSocket& udpSocket) : udpSocket(udpSocket) {}

    void UDPControlTransport::send(std::span<const std::uint8_t> packet, const Endpoint& to) noexcept {
        udpSocket.sendTo(packet, to);
    }
}

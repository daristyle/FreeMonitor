#pragma once

#include "Error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace fm {
    struct Endpoint {
        std::uint32_t address = 0;
        std::uint16_t port = 0;

        friend bool operator==(const Endpoint&, const Endpoint&) = default;
    };

    constexpr std::uint32_t makeIPAddress(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
        return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
               (static_cast<std::uint32_t>(c) << 8) | static_cast<std::uint32_t>(d);
    }

    constexpr std::uint32_t ANY_IP_ADDRESS = makeIPAddress(0, 0, 0, 0);
    constexpr std::uint32_t LOOPBACK_IP_ADDRESS = makeIPAddress(127, 0, 0, 1);

    enum class SendStatus : std::uint8_t {
        SENT,
        WOULD_BLOCK,
        SEND_FAILED,
    };

    enum class ReceiveStatus : std::uint8_t {
        RECEIVED,
        NO_DATAGRAM,
        TRUNCATED,
        RECEIVE_FAILED,
    };

    struct ReceiveResult {
        ReceiveStatus status = ReceiveStatus::NO_DATAGRAM;
        std::size_t size = 0;
    };

    class UDPSocket {
    private:
        static constexpr std::uintptr_t INVALID_SOCKET_HANDLE = ~std::uintptr_t{0};

        std::uintptr_t socketHandle = INVALID_SOCKET_HANDLE;

        explicit UDPSocket(std::uintptr_t socketHandle) : socketHandle(socketHandle) {}

        void close() noexcept;

    public:
        static Result<UDPSocket> open(const Endpoint& bindTo);

        ~UDPSocket() {
            close();
        }

        UDPSocket(UDPSocket&& other) noexcept
            : socketHandle(std::exchange(other.socketHandle, INVALID_SOCKET_HANDLE)) {}

        UDPSocket& operator=(UDPSocket&& other) noexcept {
            if (this != &other) {
                close();
                socketHandle = std::exchange(other.socketHandle, INVALID_SOCKET_HANDLE);
            }
            return *this;
        }

        UDPSocket(const UDPSocket&) = delete;
        UDPSocket& operator=(const UDPSocket&) = delete;

        Result<Endpoint> localEndpoint() const;
        Result<int> setReceiveBufferSize(int bytes);
        Result<int> setSendBufferSize(int bytes);

        SendStatus sendTo(std::span<const std::uint8_t> data, const Endpoint& to) noexcept;
        ReceiveResult receiveFrom(std::span<std::uint8_t> buffer, Endpoint& from,
                                  std::chrono::milliseconds timeout) noexcept;
    };
}

#include "UDPSocket.hpp"

#include <winsock2.h>

#include <mstcpip.h>
#include <ws2tcpip.h>

#include <climits>
#include <string>
#include <system_error>

namespace fm {
    namespace {
        SOCKET nativeHandle(std::uintptr_t socketHandle) {
            return static_cast<SOCKET>(socketHandle);
        }

        sockaddr_in toSockaddr(const Endpoint& endpoint) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(endpoint.port);
            address.sin_addr.s_addr = htonl(endpoint.address);
            return address;
        }

        Endpoint toEndpoint(const sockaddr_in& address) {
            return Endpoint{ntohl(address.sin_addr.s_addr), ntohs(address.sin_port)};
        }

        Error systemError(const char* operation, int code) {
            const ErrorCode errorCode = code == WSAEACCES ? ErrorCode::PERMISSION_DENIED : ErrorCode::DEVICE_FAILURE;
            return Error{errorCode, std::string(operation) + ": " + std::system_category().message(code)};
        }

        Error lastSystemError(const char* operation) {
            return systemError(operation, ::WSAGetLastError());
        }

        int toPollTimeout(std::chrono::milliseconds timeout) {
            if (timeout.count() <= 0) {
                return 0;
            }
            if (timeout.count() > INT_MAX) {
                return INT_MAX;
            }
            return static_cast<int>(timeout.count());
        }

        bool isTransient(int code) {
            return code == WSAEWOULDBLOCK || code == WSAEINTR || code == WSAECONNRESET || code == WSAENETRESET;
        }

        Result<> disableICMPErrorReport(SOCKET handle, DWORD controlCode, const char* operation) {
            BOOL reportError = FALSE;
            DWORD bytesReturned = 0;
            if (::WSAIoctl(handle, controlCode, &reportError, static_cast<DWORD>(sizeof(reportError)), nullptr, 0,
                           &bytesReturned, nullptr, nullptr) != 0) {
                return std::unexpected(lastSystemError(operation));
            }
            return {};
        }

        ReceiveResult receiveNow(SOCKET handle, std::span<std::uint8_t> buffer, Endpoint& from) {
            sockaddr_in address{};
            int addressSize = static_cast<int>(sizeof(address));
            const int bufferSize = buffer.size() > INT_MAX ? INT_MAX : static_cast<int>(buffer.size());
            const int received = ::recvfrom(handle, reinterpret_cast<char*>(buffer.data()), bufferSize, 0,
                                            reinterpret_cast<sockaddr*>(&address), &addressSize);
            if (received == SOCKET_ERROR) {
                const int code = ::WSAGetLastError();
                if (code == WSAEMSGSIZE) {
                    from = toEndpoint(address);
                    return {ReceiveStatus::TRUNCATED, static_cast<std::size_t>(bufferSize)};
                }
                return {isTransient(code) ? ReceiveStatus::NO_DATAGRAM : ReceiveStatus::RECEIVE_FAILED, 0};
            }

            from = toEndpoint(address);
            return {ReceiveStatus::RECEIVED, static_cast<std::size_t>(received)};
        }

        Result<int> setBufferSize(SOCKET handle, int option, int bytes, const char* operation) {
            if (::setsockopt(handle, SOL_SOCKET, option, reinterpret_cast<const char*>(&bytes),
                             static_cast<int>(sizeof(bytes))) != 0) {
                return std::unexpected(lastSystemError(operation));
            }
            int actualBytes = 0;
            int optionSize = static_cast<int>(sizeof(actualBytes));
            if (::getsockopt(handle, SOL_SOCKET, option, reinterpret_cast<char*>(&actualBytes), &optionSize) != 0) {
                return std::unexpected(lastSystemError(operation));
            }
            return actualBytes;
        }
    }

    Result<UDPSocket> UDPSocket::open(const Endpoint& bindTo) {
        WSADATA wsaData{};
        if (const int code = ::WSAStartup(MAKEWORD(2, 2), &wsaData); code != 0) {
            return std::unexpected(systemError("WSAStartup", code));
        }

        const SOCKET handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (handle == INVALID_SOCKET) {
            const Error error = lastSystemError("socket");
            ::WSACleanup();
            return std::unexpected(error);
        }
        UDPSocket udpSocket(static_cast<std::uintptr_t>(handle));

        u_long nonBlocking = 1;
        if (::ioctlsocket(handle, FIONBIO, &nonBlocking) != 0) {
            return std::unexpected(lastSystemError("ioctlsocket(FIONBIO)"));
        }

        if (auto disabled = disableICMPErrorReport(handle, SIO_UDP_CONNRESET, "WSAIoctl(SIO_UDP_CONNRESET)");
            !disabled) {
            return std::unexpected(disabled.error());
        }
        if (auto disabled = disableICMPErrorReport(handle, SIO_UDP_NETRESET, "WSAIoctl(SIO_UDP_NETRESET)"); !disabled) {
            return std::unexpected(disabled.error());
        }

        const sockaddr_in address = toSockaddr(bindTo);
        if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
            return std::unexpected(lastSystemError("bind"));
        }
        return udpSocket;
    }

    void UDPSocket::close() noexcept {
        if (socketHandle != INVALID_SOCKET_HANDLE) {
            ::closesocket(nativeHandle(socketHandle));
            ::WSACleanup();
            socketHandle = INVALID_SOCKET_HANDLE;
        }
    }

    Result<Endpoint> UDPSocket::localEndpoint() const {
        sockaddr_in address{};
        int addressSize = static_cast<int>(sizeof(address));
        if (::getsockname(nativeHandle(socketHandle), reinterpret_cast<sockaddr*>(&address), &addressSize) != 0) {
            return std::unexpected(lastSystemError("getsockname"));
        }
        return toEndpoint(address);
    }

    Result<int> UDPSocket::setReceiveBufferSize(int bytes) {
        return setBufferSize(nativeHandle(socketHandle), SO_RCVBUF, bytes, "setsockopt(SO_RCVBUF)");
    }

    Result<int> UDPSocket::setSendBufferSize(int bytes) {
        return setBufferSize(nativeHandle(socketHandle), SO_SNDBUF, bytes, "setsockopt(SO_SNDBUF)");
    }

    SendStatus UDPSocket::sendTo(std::span<const std::uint8_t> data, const Endpoint& to) noexcept {
        if (data.size() > INT_MAX) {
            return SendStatus::SEND_FAILED;
        }
        const sockaddr_in address = toSockaddr(to);
        const int sent = ::sendto(nativeHandle(socketHandle), reinterpret_cast<const char*>(data.data()),
                                  static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&address),
                                  static_cast<int>(sizeof(address)));
        if (sent != SOCKET_ERROR) {
            return SendStatus::SENT;
        }
        const int code = ::WSAGetLastError();
        if (code == WSAEWOULDBLOCK || code == WSAENOBUFS || code == WSAEINTR) {
            return SendStatus::WOULD_BLOCK;
        }
        return SendStatus::SEND_FAILED;
    }

    ReceiveResult UDPSocket::receiveFrom(std::span<std::uint8_t> buffer, Endpoint& from,
                                         std::chrono::milliseconds timeout) noexcept {
        const SOCKET handle = nativeHandle(socketHandle);
        const ReceiveResult immediate = receiveNow(handle, buffer, from);
        if (immediate.status != ReceiveStatus::NO_DATAGRAM || timeout.count() <= 0) {
            return immediate;
        }

        WSAPOLLFD pollEntry{handle, POLLRDNORM, 0};
        const int ready = ::WSAPoll(&pollEntry, 1, toPollTimeout(timeout));
        if (ready == 0) {
            return {ReceiveStatus::NO_DATAGRAM, 0};
        }
        if (ready == SOCKET_ERROR) {
            return {ReceiveStatus::RECEIVE_FAILED, 0};
        }
        return receiveNow(handle, buffer, from);
    }
}

#include "UDPSocket.hpp"

#include "posix/ReceiveTimestamp.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>

namespace fm {
    namespace {
        int nativeHandle(std::uintptr_t socketHandle) {
            return static_cast<int>(socketHandle);
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

        Error systemError(const char* operation) {
            const int code = errno;
            const ErrorCode errorCode =
                    code == EACCES || code == EPERM ? ErrorCode::PERMISSION_DENIED : ErrorCode::DEVICE_FAILURE;
            return Error{errorCode, std::string(operation) + ": " + std::system_category().message(code)};
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
            return code == EAGAIN || code == EWOULDBLOCK || code == EINTR;
        }

        std::chrono::steady_clock::time_point arrivalTime(msghdr& message,
                                                          std::chrono::steady_clock::time_point readTime) {
            for (cmsghdr* control = CMSG_FIRSTHDR(&message); control != nullptr;
                 control = CMSG_NXTHDR(&message, control)) {
                if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_TIMESTAMPNS) {
                    continue;
                }
                timespec kernelTime{};
                std::memcpy(&kernelTime, CMSG_DATA(control), sizeof(kernelTime));
                timespec wallTime{};
                if (::clock_gettime(CLOCK_REALTIME, &wallTime) != 0) {
                    return readTime;
                }
                return arrivalFromKernelTime(readTime, wallTime, kernelTime);
            }
            return readTime;
        }

        ReceiveResult receiveNow(int handle, std::span<std::uint8_t> buffer, Endpoint& from) {
            sockaddr_in address{};
            iovec data{buffer.data(), buffer.size()};
            alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(timespec))> control{};
            msghdr message{};
            message.msg_name = &address;
            message.msg_namelen = sizeof(address);
            message.msg_iov = &data;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            const ssize_t received = ::recvmsg(handle, &message, MSG_TRUNC);
            if (received < 0) {
                return {isTransient(errno) ? ReceiveStatus::NO_DATAGRAM : ReceiveStatus::RECEIVE_FAILED, 0};
            }

            from = toEndpoint(address);
            const auto arrival = arrivalTime(message, std::chrono::steady_clock::now());
            const auto datagramSize = static_cast<std::size_t>(received);
            if (datagramSize > buffer.size()) {
                return {ReceiveStatus::TRUNCATED, buffer.size(), arrival};
            }
            return {ReceiveStatus::RECEIVED, datagramSize, arrival};
        }

        Result<int> setBufferSize(int handle, int option, int bytes, const char* operation) {
            if (::setsockopt(handle, SOL_SOCKET, option, &bytes, sizeof(bytes)) != 0) {
                return std::unexpected(systemError(operation));
            }
            int actualBytes = 0;
            socklen_t optionSize = sizeof(actualBytes);
            if (::getsockopt(handle, SOL_SOCKET, option, &actualBytes, &optionSize) != 0) {
                return std::unexpected(systemError(operation));
            }
            return actualBytes;
        }
    }

    Result<UDPSocket> UDPSocket::open(const Endpoint& bindTo) {
        const int handle = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
        if (handle < 0) {
            return std::unexpected(systemError("socket"));
        }
        UDPSocket udpSocket(static_cast<std::uintptr_t>(handle));
        const int receiveTimestamps = 1;
        ::setsockopt(handle, SOL_SOCKET, SO_TIMESTAMPNS, &receiveTimestamps, sizeof(receiveTimestamps));

        const sockaddr_in address = toSockaddr(bindTo);
        if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            return std::unexpected(systemError("bind"));
        }
        return udpSocket;
    }

    void UDPSocket::close() noexcept {
        if (socketHandle != INVALID_SOCKET_HANDLE) {
            ::close(nativeHandle(socketHandle));
            socketHandle = INVALID_SOCKET_HANDLE;
        }
    }

    Result<Endpoint> UDPSocket::localEndpoint() const {
        sockaddr_in address{};
        socklen_t addressSize = sizeof(address);
        if (::getsockname(nativeHandle(socketHandle), reinterpret_cast<sockaddr*>(&address), &addressSize) != 0) {
            return std::unexpected(systemError("getsockname"));
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
        const sockaddr_in address = toSockaddr(to);
        const ssize_t sent = ::sendto(nativeHandle(socketHandle), data.data(), data.size(), 0,
                                      reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (sent >= 0) {
            return SendStatus::SENT;
        }
        if (isTransient(errno) || errno == ENOBUFS) {
            return SendStatus::WOULD_BLOCK;
        }
        return SendStatus::SEND_FAILED;
    }

    ReceiveResult UDPSocket::receiveFrom(std::span<std::uint8_t> buffer, Endpoint& from,
                                         std::chrono::milliseconds timeout) noexcept {
        const int handle = nativeHandle(socketHandle);
        const ReceiveResult immediate = receiveNow(handle, buffer, from);
        if (immediate.status != ReceiveStatus::NO_DATAGRAM || timeout.count() <= 0) {
            return immediate;
        }

        pollfd pollEntry{handle, POLLIN, 0};
        const int ready = ::poll(&pollEntry, 1, toPollTimeout(timeout));
        if (ready == 0) {
            return {ReceiveStatus::NO_DATAGRAM, 0};
        }
        if (ready < 0) {
            return {errno == EINTR ? ReceiveStatus::NO_DATAGRAM : ReceiveStatus::RECEIVE_FAILED, 0};
        }
        return receiveNow(handle, buffer, from);
    }
}

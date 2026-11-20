#include "posix/NetworkInterfaces.hpp"

#include "Error.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <memory>
#include <net/if.h>
#include <netinet/in.h>
#include <string>
#include <system_error>
#include <vector>

namespace fm {
    namespace {
        std::uint32_t ipv4Of(const sockaddr* address) noexcept {
            return ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
        }

        bool isIPv4(const sockaddr* address) noexcept {
            return address != nullptr && address->sa_family == AF_INET;
        }
    }

    Result<std::vector<std::uint32_t>> ipv4BroadcastAddresses() {
        ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0) {
            return std::unexpected(
                    Error{ErrorCode::DEVICE_FAILURE, "getifaddrs: " + std::system_category().message(errno)});
        }
        const std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)> guard(list, &::freeifaddrs);

        std::vector<std::uint32_t> addresses;
        for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
            const unsigned flags = entry->ifa_flags;
            if ((flags & IFF_UP) == 0 || (flags & IFF_BROADCAST) == 0 || (flags & IFF_LOOPBACK) != 0 ||
                !isIPv4(entry->ifa_addr) || !isIPv4(entry->ifa_netmask)) {
                continue;
            }
            const std::uint32_t address = ipv4Of(entry->ifa_addr);
            const std::uint32_t broadcast = address | ~ipv4Of(entry->ifa_netmask);
            if (broadcast != address && std::ranges::find(addresses, broadcast) == addresses.end()) {
                addresses.push_back(broadcast);
            }
        }
        return addresses;
    }
}

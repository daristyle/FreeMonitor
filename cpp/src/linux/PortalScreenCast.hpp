#pragma once

#include "Error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct sd_bus;

namespace fm {
    struct ScreenCastStream {
        int pipeWireDescriptor = -1;
        std::uint32_t nodeId = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::optional<std::string> restoreToken;
    };

    class PortalScreenCast {
    private:
        sd_bus* bus = nullptr;
        std::string senderName;
        std::string sessionHandle;
        std::uint32_t tokenCounter = 0;

        std::string nextToken(std::string_view prefix);
        Result<> createSession();
        Result<> selectSources(std::uint32_t cursorMode, bool supportsRestoreToken,
                               const std::optional<std::string>& restoreToken);
        Result<ScreenCastStream> startStream();
        Result<int> openPipeWireRemote();

    public:
        PortalScreenCast() = default;
        ~PortalScreenCast();

        PortalScreenCast(const PortalScreenCast&) = delete;
        PortalScreenCast& operator=(const PortalScreenCast&) = delete;

        Result<ScreenCastStream> start(const std::optional<std::string>& restoreToken);
        void close();

        static std::string senderNameFromUniqueName(std::string_view uniqueName);
        static std::string requestPath(std::string_view senderName, std::string_view handleToken);
    };
}

#pragma once

#include "CursorSource.hpp"
#include "Error.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fm {
    constexpr std::uint32_t DEFAULT_CURSOR_SIZE = 24;

    struct CursorTheme {
        std::string name;
        std::uint32_t size = DEFAULT_CURSOR_SIZE;
    };

    CursorTheme cursorThemeFromEnvironment();
    std::vector<std::filesystem::path> cursorSearchPath();
    std::optional<std::filesystem::path> findThemeCursor(std::span<const std::filesystem::path> searchPath,
                                                         std::string_view theme, std::string_view cursorName);
    Result<CursorImage> loadXCursorImage(const std::filesystem::path& file, std::uint32_t nominalSize);
    CursorImage builtInArrow(std::uint32_t nominalSize);
    CursorImage loadArrowCursor(std::span<const std::filesystem::path> searchPath, std::string_view theme,
                                std::uint32_t nominalSize);
}

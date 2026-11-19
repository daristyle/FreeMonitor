#include "linux/XCursorTheme.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <ranges>
#include <system_error>
#include <utility>

namespace fm {
    namespace {
        constexpr std::uint32_t IMAGE_TYPE = 0xFFFD0002;
        constexpr std::size_t FILE_HEADER_SIZE = 16;
        constexpr std::size_t TOC_ENTRY_SIZE = 12;
        constexpr std::uint32_t IMAGE_HEADER_SIZE = 36;
        constexpr std::size_t BYTES_PER_PIXEL = 4;
        constexpr std::uintmax_t MAX_FILE_SIZE = 8 * 1024 * 1024;
        constexpr int MAX_INHERIT_DEPTH = 8;
        constexpr unsigned FULL_ALPHA = 0xFF;
        constexpr std::uint32_t BUILT_IN_ARROW_SIZE = 24;
        constexpr std::array<std::string_view, 3> ARROW_NAMES{"default", "left_ptr", "arrow"};
        constexpr std::array<std::string_view, 17> BUILT_IN_ARROW{
                "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ",
                "X.....X    ", "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
                "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     X..X  ", "      XX   ",
        };

        std::uint32_t readLittleEndian(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
            return std::uint32_t{bytes[offset]} | std::uint32_t{bytes[offset + 1]} << 8U |
                   std::uint32_t{bytes[offset + 2]} << 16U | std::uint32_t{bytes[offset + 3]} << 24U;
        }

        const char* nonEmpty(const char* text) noexcept {
            return text != nullptr && *text != '\0' ? text : nullptr;
        }

        std::optional<std::uint32_t> parseSize(const char* text) {
            if (nonEmpty(text) == nullptr) {
                return std::nullopt;
            }
            const std::string_view view(text);
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), value);
            if (error != std::errc{} || end != view.data() + view.size() || value == 0 ||
                value > MAX_CURSOR_IMAGE_SIDE) {
                return std::nullopt;
            }
            return value;
        }

        std::optional<std::filesystem::path> homeDirectory() {
            if (const char* home = nonEmpty(std::getenv("HOME"))) {
                return std::filesystem::path(home);
            }
            return std::nullopt;
        }

        std::filesystem::path expandHome(std::string_view entry) {
            if (entry.starts_with("~/")) {
                if (const auto home = homeDirectory()) {
                    return *home / entry.substr(2);
                }
            }
            return std::filesystem::path(entry);
        }

        bool isPlainName(std::string_view name) noexcept {
            return !name.empty() && name != "." && name != ".." && name.find('/') == std::string_view::npos;
        }

        std::string_view trim(std::string_view text) noexcept {
            constexpr std::string_view SPACE = " \t\r";
            const auto first = text.find_first_not_of(SPACE);
            if (first == std::string_view::npos) {
                return {};
            }
            return text.substr(first, text.find_last_not_of(SPACE) - first + 1);
        }

        std::vector<std::string> inheritedThemes(const std::filesystem::path& indexFile) {
            std::ifstream input(indexFile);
            std::string line;
            while (std::getline(input, line)) {
                const std::string_view view(line);
                const auto equals = view.find('=');
                if (equals == std::string_view::npos || trim(view.substr(0, equals)) != "Inherits") {
                    continue;
                }
                std::vector<std::string> themes;
                for (const auto part : std::views::split(view.substr(equals + 1), std::string_view(","))) {
                    const std::string_view name = trim(std::string_view(part.begin(), part.end()));
                    if (!name.empty()) {
                        themes.emplace_back(name);
                    }
                }
                return themes;
            }
            return {};
        }

        std::optional<std::filesystem::path> findInTheme(std::span<const std::filesystem::path> searchPath,
                                                         std::string_view theme, std::string_view cursorName, int depth,
                                                         std::vector<std::string>& visited) {
            if (depth > MAX_INHERIT_DEPTH || !isPlainName(theme) ||
                std::ranges::find(visited, theme) != visited.end()) {
                return std::nullopt;
            }
            visited.emplace_back(theme);
            std::error_code error;
            for (const auto& base : searchPath) {
                auto candidate = base / theme / "cursors" / cursorName;
                if (std::filesystem::is_regular_file(candidate, error)) {
                    return candidate;
                }
            }
            for (const auto& base : searchPath) {
                const auto indexFile = base / theme / "index.theme";
                if (!std::filesystem::is_regular_file(indexFile, error)) {
                    continue;
                }
                for (const std::string& parent : inheritedThemes(indexFile)) {
                    if (auto found = findInTheme(searchPath, parent, cursorName, depth + 1, visited)) {
                        return found;
                    }
                }
                break;
            }
            return std::nullopt;
        }

        std::uint8_t unpremultiply(std::uint8_t channel, std::uint8_t alpha) noexcept {
            const unsigned value = (unsigned{channel} * FULL_ALPHA + alpha / 2U) / alpha;
            return static_cast<std::uint8_t>(std::min(value, FULL_ALPHA));
        }

        std::unexpected<Error> invalidFile(const std::filesystem::path& file, std::string_view reason) {
            return std::unexpected(Error{ErrorCode::INVALID_ARGUMENT,
                                         std::format("{} is not a usable XCursor file: {}", file.string(), reason)});
        }

        std::uint32_t distance(std::uint32_t size, std::uint32_t wanted) noexcept {
            return size > wanted ? size - wanted : wanted - size;
        }

        struct ImageHeader {
            std::uint32_t nominalSize = 0;
            std::size_t pixelOffset = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t hotspotX = 0;
            std::uint32_t hotspotY = 0;
        };

        std::optional<ImageHeader> imageHeaderAt(std::span<const std::uint8_t> data, std::size_t position) noexcept {
            if (position > data.size() || data.size() - position < IMAGE_HEADER_SIZE ||
                readLittleEndian(data, position) != IMAGE_HEADER_SIZE ||
                readLittleEndian(data, position + 4) != IMAGE_TYPE) {
                return std::nullopt;
            }
            const ImageHeader header{.pixelOffset = position + IMAGE_HEADER_SIZE,
                                     .width = readLittleEndian(data, position + 16),
                                     .height = readLittleEndian(data, position + 20),
                                     .hotspotX = readLittleEndian(data, position + 24),
                                     .hotspotY = readLittleEndian(data, position + 28)};
            if (header.width == 0 || header.height == 0 || header.width > MAX_CURSOR_IMAGE_SIDE ||
                header.height > MAX_CURSOR_IMAGE_SIDE || header.hotspotX >= header.width ||
                header.hotspotY >= header.height ||
                data.size() - header.pixelOffset < std::size_t{header.width} * header.height * BYTES_PER_PIXEL) {
                return std::nullopt;
            }
            return header;
        }
    }

    CursorTheme cursorThemeFromEnvironment() {
        if (const char* hyprTheme = nonEmpty(std::getenv("HYPRCURSOR_THEME"))) {
            return {hyprTheme, parseSize(std::getenv("HYPRCURSOR_SIZE")).value_or(DEFAULT_CURSOR_SIZE)};
        }
        const char* theme = nonEmpty(std::getenv("XCURSOR_THEME"));
        return {theme != nullptr ? theme : "default",
                parseSize(std::getenv("XCURSOR_SIZE")).value_or(DEFAULT_CURSOR_SIZE)};
    }

    std::vector<std::filesystem::path> cursorSearchPath() {
        std::vector<std::filesystem::path> searchPath;
        if (const char* custom = nonEmpty(std::getenv("XCURSOR_PATH"))) {
            for (const auto part : std::views::split(std::string_view(custom), std::string_view(":"))) {
                const std::string_view entry(part.begin(), part.end());
                if (!entry.empty()) {
                    searchPath.push_back(expandHome(entry));
                }
            }
            return searchPath;
        }
        const auto home = homeDirectory();
        if (const char* dataHome = nonEmpty(std::getenv("XDG_DATA_HOME"))) {
            searchPath.push_back(std::filesystem::path(dataHome) / "icons");
        } else if (home) {
            searchPath.push_back(*home / ".local/share/icons");
        }
        if (home) {
            searchPath.push_back(*home / ".icons");
        }
        searchPath.emplace_back("/usr/share/icons");
        searchPath.emplace_back("/usr/share/pixmaps");
        return searchPath;
    }

    std::optional<std::filesystem::path> findThemeCursor(std::span<const std::filesystem::path> searchPath,
                                                         std::string_view theme, std::string_view cursorName) {
        if (!isPlainName(cursorName)) {
            return std::nullopt;
        }
        std::vector<std::string> visited;
        return findInTheme(searchPath, theme, cursorName, 0, visited);
    }

    Result<CursorImage> loadXCursorImage(const std::filesystem::path& file, std::uint32_t nominalSize) {
        std::error_code error;
        const std::uintmax_t fileSize = std::filesystem::file_size(file, error);
        if (error) {
            return invalidFile(file, error.message());
        }
        if (fileSize < FILE_HEADER_SIZE || fileSize > MAX_FILE_SIZE) {
            return invalidFile(file, std::format("its size is {} bytes", fileSize));
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
        std::ifstream input(file, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return invalidFile(file, "reading it failed");
        }
        const std::span<const std::uint8_t> data(bytes);
        if (std::memcmp(data.data(), "Xcur", 4) != 0) {
            return invalidFile(file, "it does not start with Xcur");
        }

        const std::uint32_t headerSize = readLittleEndian(data, 4);
        const std::uint32_t tocCount = readLittleEndian(data, 12);
        if (headerSize < FILE_HEADER_SIZE || headerSize > data.size() ||
            tocCount > (data.size() - headerSize) / TOC_ENTRY_SIZE) {
            return invalidFile(file, "its table of contents is out of range");
        }

        std::optional<ImageHeader> best;
        for (std::uint32_t index = 0; index < tocCount; ++index) {
            const std::size_t entry = headerSize + std::size_t{index} * TOC_ENTRY_SIZE;
            if (readLittleEndian(data, entry) != IMAGE_TYPE) {
                continue;
            }
            const std::uint32_t size = readLittleEndian(data, entry + 4);
            if (best && distance(size, nominalSize) >= distance(best->nominalSize, nominalSize)) {
                continue;
            }
            if (auto header = imageHeaderAt(data, readLittleEndian(data, entry + 8))) {
                header->nominalSize = size;
                best = header;
            }
        }
        if (!best) {
            return invalidFile(file, "it has no usable image");
        }

        const std::size_t pixelBytes = std::size_t{best->width} * best->height * BYTES_PER_PIXEL;
        CursorImage image{.width = static_cast<std::uint16_t>(best->width),
                          .height = static_cast<std::uint16_t>(best->height),
                          .hotspotX = static_cast<std::uint16_t>(best->hotspotX),
                          .hotspotY = static_cast<std::uint16_t>(best->hotspotY),
                          .pixels = std::vector<std::uint8_t>(pixelBytes)};
        const auto source = data.subspan(best->pixelOffset, pixelBytes);
        for (std::size_t offset = 0; offset < pixelBytes; offset += BYTES_PER_PIXEL) {
            const std::uint8_t alpha = source[offset + 3];
            if (alpha == 0) {
                continue;
            }
            for (std::size_t channel = 0; channel < 3; ++channel) {
                image.pixels[offset + channel] = unpremultiply(source[offset + channel], alpha);
            }
            image.pixels[offset + 3] = alpha;
        }
        return image;
    }

    CursorImage builtInArrow(std::uint32_t nominalSize) {
        const std::size_t rows = BUILT_IN_ARROW.size();
        const std::size_t columns = BUILT_IN_ARROW.front().size();
        const std::size_t scale = std::clamp<std::size_t>((nominalSize + BUILT_IN_ARROW_SIZE / 2) / BUILT_IN_ARROW_SIZE,
                                                          1, MAX_CURSOR_IMAGE_SIDE / rows);
        const std::size_t width = columns * scale;
        const std::size_t height = rows * scale;
        CursorImage image{.width = static_cast<std::uint16_t>(width),
                          .height = static_cast<std::uint16_t>(height),
                          .pixels = std::vector<std::uint8_t>(width * height * BYTES_PER_PIXEL)};
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const char cell = BUILT_IN_ARROW[y / scale][x / scale];
                if (cell == ' ') {
                    continue;
                }
                const std::uint8_t shade = cell == '.' ? 0xFF : 0x00;
                std::uint8_t* pixel = image.pixels.data() + (y * width + x) * BYTES_PER_PIXEL;
                pixel[0] = shade;
                pixel[1] = shade;
                pixel[2] = shade;
                pixel[3] = static_cast<std::uint8_t>(FULL_ALPHA);
            }
        }
        return image;
    }

    CursorImage loadArrowCursor(std::span<const std::filesystem::path> searchPath, std::string_view theme,
                                std::uint32_t nominalSize) {
        for (const std::string_view name : ARROW_NAMES) {
            if (const auto file = findThemeCursor(searchPath, theme, name)) {
                if (auto image = loadXCursorImage(*file, nominalSize)) {
                    return std::move(*image);
                }
            }
        }
        return builtInArrow(nominalSize);
    }
}

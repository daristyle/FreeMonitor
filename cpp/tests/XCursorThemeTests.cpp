#include "ScopedEnvironment.hpp"
#include "TemporaryDirectory.hpp"
#include "linux/XCursorTheme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
    constexpr std::uint32_t IMAGE_TYPE = 0xFFFD0002;
    constexpr std::uint32_t IMAGE_HEADER_SIZE = 36;

    struct TestImage {
        std::uint32_t nominalSize = 24;
        std::uint32_t width = 24;
        std::uint32_t height = 24;
        std::uint32_t hotspotX = 0;
        std::uint32_t hotspotY = 0;
        std::vector<std::uint32_t> pixels;
    };

    TestImage solidImage(std::uint32_t size, std::uint32_t hotspotX, std::uint32_t hotspotY, std::uint32_t argb) {
        return TestImage{.nominalSize = size,
                         .width = size,
                         .height = size,
                         .hotspotX = hotspotX,
                         .hotspotY = hotspotY,
                         .pixels = std::vector<std::uint32_t>(std::size_t{size} * size, argb)};
    }

    void append(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    std::vector<std::uint8_t> xcursorFile(const std::vector<TestImage>& images) {
        std::vector<std::uint8_t> bytes{'X', 'c', 'u', 'r'};
        append(bytes, 16);
        append(bytes, 0x10000);
        append(bytes, static_cast<std::uint32_t>(images.size()));
        std::uint32_t position = 16 + 12 * static_cast<std::uint32_t>(images.size());
        for (const TestImage& image : images) {
            append(bytes, IMAGE_TYPE);
            append(bytes, image.nominalSize);
            append(bytes, position);
            position += IMAGE_HEADER_SIZE + 4 * static_cast<std::uint32_t>(image.pixels.size());
        }
        for (const TestImage& image : images) {
            for (const std::uint32_t field : {IMAGE_HEADER_SIZE, IMAGE_TYPE, image.nominalSize, 1U, image.width,
                                              image.height, image.hotspotX, image.hotspotY, 0U}) {
                append(bytes, field);
            }
            for (const std::uint32_t pixel : image.pixels) {
                append(bytes, pixel);
            }
        }
        return bytes;
    }

    void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void writeText(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    std::array<std::uint8_t, 4> pixelAt(const fm::CursorImage& image, std::size_t x, std::size_t y) {
        const std::size_t offset = (y * image.width + x) * 4;
        return {image.pixels[offset], image.pixels[offset + 1], image.pixels[offset + 2], image.pixels[offset + 3]};
    }
}

TEST_CASE("the image nearest the wanted size is loaded with its hotspot") {
    TemporaryDirectory directory;
    const auto file = directory.path() / "left_ptr";
    writeFile(file, xcursorFile({solidImage(24, 5, 1, 0xFF000000), solidImage(32, 7, 2, 0xFF000000),
                                 solidImage(48, 10, 3, 0xFF000000)}));

    const auto exact = fm::loadXCursorImage(file, 24);
    REQUIRE(exact);
    CHECK(exact->width == 24);
    CHECK(exact->hotspotX == 5);
    CHECK(exact->hotspotY == 1);

    const auto nearer = fm::loadXCursorImage(file, 30);
    REQUIRE(nearer);
    CHECK(nearer->width == 32);
    CHECK(nearer->hotspotX == 7);
    CHECK(nearer->pixels.size() == std::size_t{32} * 32 * 4);

    const auto tie = fm::loadXCursorImage(file, 40);
    REQUIRE(tie);
    CHECK(tie->width == 32);

    const auto large = fm::loadXCursorImage(file, 96);
    REQUIRE(large);
    CHECK(large->width == 48);
}

TEST_CASE("premultiplied XCursor pixels become straight BGRA") {
    TemporaryDirectory directory;
    const auto file = directory.path() / "default";
    TestImage image{.nominalSize = 24,
                    .width = 4,
                    .height = 1,
                    .hotspotX = 0,
                    .hotspotY = 0,
                    .pixels = {0x80402010, 0x00000000, 0xFF112233, 0x10FF0000}};
    writeFile(file, xcursorFile({image}));

    const auto loaded = fm::loadXCursorImage(file, 24);
    REQUIRE(loaded);
    CHECK(pixelAt(*loaded, 0, 0) == std::array<std::uint8_t, 4>{32, 64, 128, 128});
    CHECK(pixelAt(*loaded, 1, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    CHECK(pixelAt(*loaded, 2, 0) == std::array<std::uint8_t, 4>{0x33, 0x22, 0x11, 0xFF});
    CHECK(pixelAt(*loaded, 3, 0) == std::array<std::uint8_t, 4>{0, 0, 255, 0x10});
}

TEST_CASE("a broken image is skipped in favour of the nearest usable one") {
    TemporaryDirectory directory;
    const auto file = directory.path() / "left_ptr";

    SECTION("an image with its hotspot outside") {
        writeFile(file, xcursorFile({solidImage(24, 24, 1, 0xFF000000), solidImage(32, 7, 2, 0xFF000000)}));
    }
    SECTION("a table of contents entry pointing past the end of the file") {
        auto bytes = xcursorFile({solidImage(24, 5, 1, 0xFF000000), solidImage(32, 7, 2, 0xFF000000)});
        bytes[24] = 0xF0;
        bytes[25] = 0xFF;
        bytes[26] = 0xFF;
        bytes[27] = 0xFF;
        writeFile(file, bytes);
    }
    const auto loaded = fm::loadXCursorImage(file, 24);
    REQUIRE(loaded);
    CHECK(loaded->width == 32);
    CHECK(loaded->hotspotX == 7);
}

TEST_CASE("malformed XCursor files are refused") {
    TemporaryDirectory directory;
    const auto file = directory.path() / "broken";
    auto bytes = xcursorFile({solidImage(24, 5, 1, 0xFF000000)});

    SECTION("wrong magic") {
        bytes[0] = 'Y';
    }
    SECTION("pixels cut off") {
        bytes.resize(bytes.size() - 1);
    }
    SECTION("hotspot outside the image") {
        bytes = xcursorFile({solidImage(24, 24, 1, 0xFF000000)});
    }
    SECTION("an image wider than the protocol allows") {
        TestImage wide{.nominalSize = 24, .width = 257, .height = 1, .pixels = std::vector<std::uint32_t>(257)};
        bytes = xcursorFile({wide});
    }
    SECTION("no images") {
        bytes = xcursorFile({});
    }
    SECTION("a table of contents longer than the file") {
        bytes[12] = 0xFF;
    }
    writeFile(file, bytes);
    CHECK_FALSE(fm::loadXCursorImage(file, 24));
}

TEST_CASE("theme cursors are found in search path order and through inherited themes") {
    TemporaryDirectory directory;
    const auto first = directory.path() / "one";
    const auto second = directory.path() / "two";
    const std::vector<std::filesystem::path> searchPath{first, second};
    const auto cursor = xcursorFile({solidImage(24, 5, 1, 0xFF000000)});

    writeFile(first / "Child" / "cursors" / "text", cursor);
    writeFile(second / "Child" / "cursors" / "text", cursor);
    writeText(second / "Child" / "index.theme", "[Icon Theme]\nName=Child\nInherits = Missing, Parent\n");
    writeFile(first / "Parent" / "cursors" / "left_ptr", cursor);
    writeText(first / "LoopA" / "index.theme", "[Icon Theme]\nInherits=LoopB\n");
    writeText(first / "LoopB" / "index.theme", "[Icon Theme]\nInherits=LoopA\n");

    CHECK(fm::findThemeCursor(searchPath, "Child", "text") == first / "Child" / "cursors" / "text");
    CHECK(fm::findThemeCursor(searchPath, "Child", "left_ptr") == first / "Parent" / "cursors" / "left_ptr");
    CHECK_FALSE(fm::findThemeCursor(searchPath, "Child", "watch"));
    CHECK_FALSE(fm::findThemeCursor(searchPath, "LoopA", "left_ptr"));
    CHECK_FALSE(fm::findThemeCursor(searchPath, "../one/Parent", "left_ptr"));
    CHECK_FALSE(fm::findThemeCursor(searchPath, "Parent", "../cursors/left_ptr"));
}

TEST_CASE("the arrow comes from the theme, or is built in when the theme has none") {
    TemporaryDirectory directory;
    const std::vector<std::filesystem::path> searchPath{directory.path()};
    writeFile(directory.path() / "Theme" / "cursors" / "left_ptr", xcursorFile({solidImage(24, 5, 1, 0xFF000000)}));

    const fm::CursorImage themed = fm::loadArrowCursor(searchPath, "Theme", 24);
    CHECK(themed.width == 24);
    CHECK(themed.hotspotX == 5);

    const fm::CursorImage builtIn = fm::loadArrowCursor(searchPath, "NoSuchTheme", 24);
    CHECK(builtIn.width == 11);
    CHECK(builtIn.height == 17);
    CHECK(builtIn.hotspotX == 0);
    CHECK(builtIn.hotspotY == 0);
    CHECK(pixelAt(builtIn, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255});
    CHECK(pixelAt(builtIn, 1, 2) == std::array<std::uint8_t, 4>{255, 255, 255, 255});
    CHECK(pixelAt(builtIn, 10, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
}

TEST_CASE("the built-in arrow grows with the cursor size but stays within the protocol's limit") {
    CHECK(fm::builtInArrow(24).width == 11);
    CHECK(fm::builtInArrow(48).width == 22);
    CHECK(fm::builtInArrow(48).height == 34);
    const fm::CursorImage huge = fm::builtInArrow(100000);
    CHECK(huge.width <= fm::MAX_CURSOR_IMAGE_SIDE);
    CHECK(huge.height <= fm::MAX_CURSOR_IMAGE_SIDE);
    CHECK(huge.pixels.size() == std::size_t{huge.width} * huge.height * 4);
}

TEST_CASE("the cursor theme and size come from the environment") {
    SECTION("Hyprland's own pair first, like Hyprland") {
        ScopedEnvironment theme("XCURSOR_THEME", "Bibata-Modern-Ice");
        ScopedEnvironment size("XCURSOR_SIZE", "32");
        ScopedEnvironment hyprTheme("HYPRCURSOR_THEME", "Other");
        ScopedEnvironment hyprSize("HYPRCURSOR_SIZE", "48");
        const auto chosen = fm::cursorThemeFromEnvironment();
        CHECK(chosen.name == "Other");
        CHECK(chosen.size == 48);
    }
    SECTION("the size comes from the same pair as the theme") {
        ScopedEnvironment theme("XCURSOR_THEME", "Bibata-Modern-Ice");
        ScopedEnvironment size("XCURSOR_SIZE", "32");
        ScopedEnvironment hyprTheme("HYPRCURSOR_THEME", "Other");
        ScopedEnvironment hyprSize("HYPRCURSOR_SIZE", nullptr);
        CHECK(fm::cursorThemeFromEnvironment().size == fm::DEFAULT_CURSOR_SIZE);
    }
    SECTION("the XCursor pair when Hyprland's theme is not set") {
        ScopedEnvironment theme("XCURSOR_THEME", "Bibata-Modern-Ice");
        ScopedEnvironment size("XCURSOR_SIZE", nullptr);
        ScopedEnvironment hyprTheme("HYPRCURSOR_THEME", nullptr);
        ScopedEnvironment hyprSize("HYPRCURSOR_SIZE", "32");
        const auto chosen = fm::cursorThemeFromEnvironment();
        CHECK(chosen.name == "Bibata-Modern-Ice");
        CHECK(chosen.size == fm::DEFAULT_CURSOR_SIZE);
    }
    SECTION("defaults when nothing usable is set") {
        ScopedEnvironment theme("XCURSOR_THEME", "");
        ScopedEnvironment size("XCURSOR_SIZE", "big");
        ScopedEnvironment hyprTheme("HYPRCURSOR_THEME", nullptr);
        ScopedEnvironment hyprSize("HYPRCURSOR_SIZE", "0");
        const auto chosen = fm::cursorThemeFromEnvironment();
        CHECK(chosen.name == "default");
        CHECK(chosen.size == fm::DEFAULT_CURSOR_SIZE);
    }
}

TEST_CASE("the cursor search path follows XCURSOR_PATH or the usual folders") {
    ScopedEnvironment home("HOME", "/home/test");
    SECTION("XCURSOR_PATH replaces the defaults and expands ~") {
        ScopedEnvironment path("XCURSOR_PATH", "~/icons:/opt/cursors::");
        CHECK(fm::cursorSearchPath() == std::vector<std::filesystem::path>{"/home/test/icons", "/opt/cursors"});
    }
    SECTION("the defaults") {
        ScopedEnvironment path("XCURSOR_PATH", nullptr);
        ScopedEnvironment dataHome("XDG_DATA_HOME", nullptr);
        CHECK(fm::cursorSearchPath() == std::vector<std::filesystem::path>{"/home/test/.local/share/icons",
                                                                           "/home/test/.icons", "/usr/share/icons",
                                                                           "/usr/share/pixmaps"});
    }
    SECTION("XDG_DATA_HOME moves the first folder") {
        ScopedEnvironment path("XCURSOR_PATH", nullptr);
        ScopedEnvironment dataHome("XDG_DATA_HOME", "/data");
        CHECK(fm::cursorSearchPath().front() == std::filesystem::path("/data/icons"));
    }
}

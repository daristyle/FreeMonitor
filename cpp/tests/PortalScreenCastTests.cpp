#include "linux/PortalScreenCast.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the portal sender name drops the colon and replaces dots") {
    CHECK(fm::PortalScreenCast::senderNameFromUniqueName(":1.234") == "1_234");
    CHECK(fm::PortalScreenCast::senderNameFromUniqueName(":1.2.3") == "1_2_3");
    CHECK(fm::PortalScreenCast::senderNameFromUniqueName("1.5") == "1_5");
}

TEST_CASE("the request path follows the portal's documented layout") {
    CHECK(fm::PortalScreenCast::requestPath("1_234", "fm_request_1") ==
          "/org/freedesktop/portal/desktop/request/1_234/fm_request_1");
}

TEST_CASE("a portal that was never started can be closed and destroyed safely") {
    fm::PortalScreenCast portal;
    portal.close();
    portal.close();
}

TEST_CASE(
        "the portal cursor mode follows the wish, and never leaves the cursor out unasked when embedding is offered") {
    constexpr std::uint32_t HIDDEN = fm::PortalScreenCast::CURSOR_MODE_HIDDEN;
    constexpr std::uint32_t EMBEDDED = fm::PortalScreenCast::CURSOR_MODE_EMBEDDED;
    constexpr std::uint32_t METADATA = 4;
    CHECK(fm::PortalScreenCast::chooseCursorMode(HIDDEN | EMBEDDED, true) == HIDDEN);
    CHECK(fm::PortalScreenCast::chooseCursorMode(HIDDEN | EMBEDDED, false) == EMBEDDED);
    CHECK(fm::PortalScreenCast::chooseCursorMode(HIDDEN | EMBEDDED | METADATA, true) == HIDDEN);
    CHECK(fm::PortalScreenCast::chooseCursorMode(EMBEDDED, true) == EMBEDDED);
    CHECK(fm::PortalScreenCast::chooseCursorMode(HIDDEN, false) == HIDDEN);
    CHECK(fm::PortalScreenCast::chooseCursorMode(0, false) == HIDDEN);
}

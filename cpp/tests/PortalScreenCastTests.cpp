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

#include "FakeHyprlandServer.hpp"
#include "linux/HyprlandVirtualDisplay.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace std::chrono_literals;

namespace {
    constexpr std::string_view OUTPUT_NAME = "FREEMONITOR-1";
    constexpr std::chrono::milliseconds SHORT_APPLY_TIMEOUT{100};

    std::string laptopAt(std::int32_t x, std::int32_t y) {
        return std::format("Monitor eDP-1 (ID 0):\n"
                           "\t1920x1080@144.06300 at {}x{}\n"
                           "\tdescription: Example Panel 15\n"
                           "\tmake: Example\n"
                           "\tmodel: Panel 15 \n"
                           "\tphysical size (mm): 340x190\n"
                           "\tserial: \n"
                           "\tactive workspace: 2 (2)\n"
                           "\tspecial workspace: 0 ()\n"
                           "\treserved: 0 45 0 0\n"
                           "\tscale: 1.00\n"
                           "\ttransform: 0\n"
                           "\tfocused: yes\n"
                           "\tdpmsStatus: 1\n"
                           "\tvrr: false\n"
                           "\n",
                           x, y);
    }

    std::string outputAt(std::uint32_t width, std::uint32_t height, std::int32_t x, std::int32_t y) {
        return std::format("Monitor FREEMONITOR-1 (ID 1):\n"
                           "\t{}x{}@60.00000 at {}x{}\n"
                           "\tscale: 1.00\n"
                           "\ttransform: 0\n"
                           "\n",
                           width, height, x, y);
    }

    const std::string LAPTOP_ONLY = laptopAt(0, 0);
    const std::string TABLET_APPLIED = LAPTOP_ONLY + outputAt(2560, 1600, 1920, 0);
    const std::string TABLET_NOT_APPLIED_YET = LAPTOP_ONLY + outputAt(1920, 1080, 1920, 0);

    constexpr fm::DisplayMode TABLET_MODE{.width = 2560, .height = 1600, .refreshRateHz = 60};

    const std::vector<std::string> CREATE_REQUESTS{
            "output remove FREEMONITOR-1",
            "monitors",
            "output create headless FREEMONITOR-1",
            "keyword monitor FREEMONITOR-1,2560x1600@60,1920x0,1",
            "monitors",
            "workspaces",
    };

    const std::string LAPTOP_WORKSPACE = "workspace ID 1 (1) on monitor eDP-1:\n\tmonitorID: 0\n\twindows: 1\n\n";

    std::string workspaceOnTablet(int id, int windows) {
        return std::format("workspace ID {} ({}) on monitor FREEMONITOR-1:\n"
                           "\tmonitorID: 1\n"
                           "\twindows: {}\n"
                           "\thasfullscreen: 0\n"
                           "\tlastwindow: 0x5564b10ed860\n"
                           "\tlastwindowtitle: a title\n"
                           "\tispersistent: 0\n"
                           "\ttiledLayout: dwindle\n"
                           "\n",
                           id, id, windows);
    }

    std::string ruleFor(std::string_view workspace, std::string_view monitor) {
        return std::format("Workspace rule {}:\n\tenabled: true\n\tmonitor: {}\n\tdefault: <unset>\n\n", workspace,
                           monitor);
    }

    struct FakeCompositor {
        std::vector<std::string> monitorsReplies{LAPTOP_ONLY, TABLET_APPLIED};
        std::string createReply = "ok";
        std::string keywordReply = "ok";
        std::string workspacesReply = LAPTOP_WORKSPACE;
        std::string rulesReply;
        std::size_t monitorsCalls = 0;

        std::optional<std::string> operator()(const std::string& request) {
            if (request.starts_with("output remove ")) {
                return std::string("output not found");
            }
            if (request == "monitors") {
                return monitorsReplies[std::min(monitorsCalls++, monitorsReplies.size() - 1)];
            }
            if (request.starts_with("output create headless ")) {
                return createReply;
            }
            if (request.starts_with("keyword monitor ")) {
                return keywordReply;
            }
            if (request == "workspaces") {
                return workspacesReply;
            }
            if (request == "workspacerules") {
                return rulesReply;
            }
            if (request.starts_with("dispatch ")) {
                return std::string("ok");
            }
            return std::string("unknown request");
        }
    };

    fm::HyprlandVirtualDisplay makeDisplay(const FakeHyprlandServer& server) {
        return fm::HyprlandVirtualDisplay(fm::HyprlandIPC(server.path()), std::string(OUTPUT_NAME),
                                          SHORT_APPLY_TIMEOUT);
    }
}

TEST_CASE("parseHyprlandMonitors reads the real monitors format") {
    const auto monitors = fm::parseHyprlandMonitors(LAPTOP_ONLY);
    REQUIRE(monitors);
    REQUIRE(monitors->size() == 1);
    const auto& laptop = (*monitors)[0];
    CHECK(laptop.name == "eDP-1");
    CHECK(laptop.modeText == "1920x1080@144.06300");
    CHECK(laptop.scaleText == "1.00");
    CHECK(laptop.transform == 0);
    CHECK(laptop.layout == fm::MonitorLayout{0, 0, 1920, 1080});
}

TEST_CASE("parseHyprlandMonitors gives the layout size after scale and rotation") {
    const std::string reply = LAPTOP_ONLY + "Monitor DP-1 (ID 1):\n"
                                            "\t2560x1600@60.00000 at 1920x0\n"
                                            "\tdescription: scaled 2x at the office\n"
                                            "\tscale: 2.00\n"
                                            "\ttransform: 0\n"
                                            "\n"
                                            "Monitor DP-2 (ID 2):\n"
                                            "\t1920x1080@60.00000 at -1080x-200\n"
                                            "\tscale: 1.00\n"
                                            "\ttransform: 1\n";
    const auto monitors = fm::parseHyprlandMonitors(reply);
    REQUIRE(monitors);
    REQUIRE(monitors->size() == 3);
    CHECK((*monitors)[1].name == "DP-1");
    CHECK((*monitors)[1].layout == fm::MonitorLayout{1920, 0, 1280, 800});
    CHECK((*monitors)[2].transform == 1);
    CHECK((*monitors)[2].layout == fm::MonitorLayout{-1080, -200, 1080, 1920});
}

TEST_CASE("parseHyprlandMonitors rejects a layout it cannot read") {
    const std::string reply = GENERATE(
            std::string("Monitor A (ID 0):\n\tgarbage at here\n"), std::string("Monitor A (ID 0):\n\tscale: 1.00\n"),
            std::string("Monitor A (ID 0):\n\t1920x1080@60 at 0x0\n\tscale: 0.00\n"),
            std::string("Monitor A (ID 0):\n\t1920x1080@60 at 0x0\n\tscale: big\n"),
            std::string("Monitor A (ID 0):\n\t1920x@60 at 0x0\n"), std::string("Monitor A:\n\t1920x1080@60 at 0x0\n"),
            std::string("Monitor  (ID 0):\n\t1920x1080@60 at 0x0\n"),
            std::string("Monitor (ID 0):\n\t1920x1080@60 at 0x0\n"));
    CAPTURE(reply);
    CHECK_FALSE(fm::parseHyprlandMonitors(reply));
}

TEST_CASE("parseHyprlandMonitors returns no monitors for an empty reply") {
    const auto monitors = fm::parseHyprlandMonitors("");
    REQUIRE(monitors);
    CHECK(monitors->empty());
}

TEST_CASE("create removes a leftover output, places the display right of the layout and checks the result") {
    FakeHyprlandServer server(FakeCompositor{});
    auto virtualDisplay = makeDisplay(server);

    const auto target = virtualDisplay.create(TABLET_MODE);
    REQUIRE(target);
    CHECK(target->outputName == OUTPUT_NAME);
    CHECK(target->width == TABLET_MODE.width);
    CHECK(target->height == TABLET_MODE.height);
    CHECK(target->cursorInPicture);
    CHECK(server.receivedRequests() == CREATE_REQUESTS);
}

TEST_CASE("create waits until Hyprland has applied the monitor rule") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_NOT_APPLIED_YET, TABLET_NOT_APPLIED_YET, TABLET_APPLIED};
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    REQUIRE(virtualDisplay.create(TABLET_MODE));
    const auto requests = server.receivedRequests();
    CHECK(std::ranges::count(requests, std::string("monitors")) == 4);
}

TEST_CASE("create fails and removes the output when the rule is never applied") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_NOT_APPLIED_YET};
    FakeHyprlandServer server(compositor);
    {
        auto virtualDisplay = makeDisplay(server);
        const auto start = std::chrono::steady_clock::now();
        const auto target = virtualDisplay.create(TABLET_MODE);
        REQUIRE_FALSE(target);
        CHECK(target.error().code == fm::ErrorCode::DEVICE_FAILURE);
        CHECK(std::chrono::steady_clock::now() - start >= SHORT_APPLY_TIMEOUT);
    }
    CHECK(server.receivedRequests().back() == "output remove FREEMONITOR-1");
}

TEST_CASE("create refuses a layout change that moves another monitor and explains the fix") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, laptopAt(4480, 0) + outputAt(2560, 1600, 1920, 0)};
    FakeHyprlandServer server(compositor);
    {
        auto virtualDisplay = makeDisplay(server);
        const auto target = virtualDisplay.create(TABLET_MODE);
        REQUIRE_FALSE(target);
        CHECK(target.error().code == fm::ErrorCode::INVALID_STATE);
        const std::string& message = target.error().message;
        CAPTURE(message);
        CHECK(message.find("eDP-1 from 0,0 to 4480,0") != std::string::npos);
        CHECK(message.find("monitor = eDP-1, 1920x1080@144.06300, 0x0, 1.00\n") != std::string::npos);
        CHECK(message.find("--position left") != std::string::npos);
    }
    const auto requests = server.receivedRequests();
    CHECK(std::ranges::find(requests, "workspaces") == requests.end());
    CHECK(requests.back() == "output remove FREEMONITOR-1");
}

TEST_CASE("an explicit position is used as given") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, LAPTOP_ONLY + outputAt(2560, 1600, -2560, 120)};
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);
    auto mode = TABLET_MODE;
    mode.placement = fm::DisplayPlacement::EXPLICIT;
    mode.positionX = -2560;
    mode.positionY = 120;

    REQUIRE(virtualDisplay.create(mode));
    CHECK(server.receivedRequests()[3] == "keyword monitor FREEMONITOR-1,2560x1600@60,-2560x120,1");
}

TEST_CASE("destroy and the destructor remove the output once") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_APPLIED, LAPTOP_ONLY, TABLET_APPLIED};
    FakeHyprlandServer server(compositor);
    {
        auto virtualDisplay = makeDisplay(server);
        REQUIRE(virtualDisplay.create(TABLET_MODE));
        virtualDisplay.destroy();
        virtualDisplay.destroy();
        REQUIRE(virtualDisplay.create(TABLET_MODE));
    }
    const auto requests = server.receivedRequests();
    REQUIRE(requests.size() == 2 * CREATE_REQUESTS.size() + 2);
    CHECK(requests[CREATE_REQUESTS.size()] == "output remove FREEMONITOR-1");
    CHECK(requests.back() == "output remove FREEMONITOR-1");
}

TEST_CASE("destroy sends nothing when no output was created") {
    FakeHyprlandServer server(FakeCompositor{});
    {
        auto virtualDisplay = makeDisplay(server);
        virtualDisplay.destroy();
    }
    CHECK(server.receivedRequests().empty());
}

TEST_CASE("stillExists follows Hyprland's monitor list") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_APPLIED, TABLET_APPLIED, LAPTOP_ONLY};
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);
    REQUIRE(virtualDisplay.create(TABLET_MODE));

    CHECK(virtualDisplay.stillExists());
    CHECK_FALSE(virtualDisplay.stillExists());
}

TEST_CASE("stillExists asks Hyprland nothing when no output was created") {
    FakeHyprlandServer server(FakeCompositor{});
    auto virtualDisplay = makeDisplay(server);

    CHECK_FALSE(virtualDisplay.stillExists());
    CHECK(server.receivedRequests().empty());
}

TEST_CASE("an unreadable monitor list does not count as a lost display") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_APPLIED, "Monitor A (ID 0):\n\tgarbage at here\n"};
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);
    REQUIRE(virtualDisplay.create(TABLET_MODE));

    CHECK(virtualDisplay.stillExists());
}

TEST_CASE("Hyprland not answering does not count as a lost display") {
    std::optional<FakeHyprlandServer> server(std::in_place, FakeCompositor{});
    auto virtualDisplay = makeDisplay(*server);
    REQUIRE(virtualDisplay.create(TABLET_MODE));

    server.reset();
    CHECK(virtualDisplay.stillExists());
}

TEST_CASE("a second display on the same output is refused and leaves the monitor alone") {
    FakeHyprlandServer server(FakeCompositor{});
    auto first = makeDisplay(server);
    REQUIRE(first.create(TABLET_MODE));
    const std::size_t requestsBefore = server.receivedRequests().size();

    auto second = makeDisplay(server);
    const auto refused = second.create(TABLET_MODE);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == fm::ErrorCode::INVALID_STATE);
    CHECK(refused.error().message.contains(OUTPUT_NAME));
    CHECK(server.receivedRequests().size() == requestsBefore);
}

TEST_CASE("the output can be taken again after destroy") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_APPLIED, LAPTOP_ONLY, TABLET_APPLIED};
    FakeHyprlandServer server(compositor);
    auto first = makeDisplay(server);
    REQUIRE(first.create(TABLET_MODE));
    first.destroy();

    auto second = makeDisplay(server);
    CHECK(second.create(TABLET_MODE));
}

TEST_CASE("the output can be taken again after a failed create") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {"Monitor A (ID 0):\n\tgarbage at here\n", LAPTOP_ONLY, TABLET_APPLIED};
    FakeHyprlandServer server(compositor);
    auto first = makeDisplay(server);
    REQUIRE_FALSE(first.create(TABLET_MODE));

    auto second = makeDisplay(server);
    CHECK(second.create(TABLET_MODE));
}

TEST_CASE("a host that dies without cleaning up leaves the output free and its leftover is removed") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_APPLIED, LAPTOP_ONLY, TABLET_APPLIED};
    FakeHyprlandServer server(compositor);

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        auto dying = makeDisplay(server);
        ::_exit(dying.create(TABLET_MODE) ? 0 : 1);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    auto next = makeDisplay(server);
    CHECK(next.create(TABLET_MODE));
    const auto requests = server.receivedRequests();
    REQUIRE(requests.size() == 2 * CREATE_REQUESTS.size());
    CHECK(requests[CREATE_REQUESTS.size()] == "output remove FREEMONITOR-1");
}

TEST_CASE("a child process does not keep the output locked") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {LAPTOP_ONLY, TABLET_APPLIED, LAPTOP_ONLY, TABLET_APPLIED};
    FakeHyprlandServer server(compositor);
    auto first = makeDisplay(server);
    REQUIRE(first.create(TABLET_MODE));

    std::array<int, 2> execDone{};
    REQUIRE(::pipe2(execDone.data(), O_CLOEXEC) == 0);
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::execlp("sleep", "sleep", "10", static_cast<char*>(nullptr));
        ::_exit(1);
    }
    ::close(execDone[1]);
    char ignored = 0;
    REQUIRE(::read(execDone[0], &ignored, 1) == 0);
    ::close(execDone[0]);

    first.destroy();
    auto second = makeDisplay(server);
    const bool taken = second.create(TABLET_MODE).has_value();

    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    CHECK(taken);
}

TEST_CASE("a different output name is not blocked") {
    FakeHyprlandServer server(FakeCompositor{});
    auto first = makeDisplay(server);
    REQUIRE(first.create(TABLET_MODE));

    fm::HyprlandVirtualDisplay other(fm::HyprlandIPC(server.path()), "FREEMONITOR-2", SHORT_APPLY_TIMEOUT);
    static_cast<void>(other.create(TABLET_MODE));
    const auto requests = server.receivedRequests();
    CHECK(std::ranges::find(requests, "output remove FREEMONITOR-2") != requests.end());
}

TEST_CASE("a lock file that cannot be opened is an error and nothing reaches Hyprland") {
    FakeHyprlandServer server(FakeCompositor{});
    fm::HyprlandVirtualDisplay virtualDisplay(fm::HyprlandIPC(server.path().parent_path() / "missing" / ".socket.sock"),
                                              std::string(OUTPUT_NAME), SHORT_APPLY_TIMEOUT);

    const auto result = virtualDisplay.create(TABLET_MODE);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == fm::ErrorCode::DEVICE_FAILURE);
    CHECK(server.receivedRequests().empty());
}

TEST_CASE("parseHyprlandMonitors reads which monitor is focused") {
    const auto monitors = fm::parseHyprlandMonitors(TABLET_APPLIED);
    REQUIRE(monitors);
    REQUIRE(monitors->size() == 2);
    CHECK((*monitors)[0].focused);
    CHECK_FALSE((*monitors)[1].focused);
}

TEST_CASE("parseHyprlandWorkspaces reads the real workspaces format") {
    const auto workspaces = fm::parseHyprlandWorkspaces(LAPTOP_WORKSPACE + workspaceOnTablet(7, 2) +
                                                        "workspace ID -98 (special:scratchpad) on monitor eDP-1:\n"
                                                        "\twindows: 1\n");
    REQUIRE(workspaces);
    REQUIRE(workspaces->size() == 3);
    CHECK((*workspaces)[0].id == 1);
    CHECK((*workspaces)[0].monitor == "eDP-1");
    CHECK((*workspaces)[0].windows == 1);
    CHECK((*workspaces)[1].id == 7);
    CHECK((*workspaces)[1].monitor == "FREEMONITOR-1");
    CHECK((*workspaces)[1].windows == 2);
    CHECK((*workspaces)[2].id == -98);
}

TEST_CASE("parseHyprlandWorkspaces rejects a list it cannot read") {
    const auto text = GENERATE(std::string("workspace ID x (7) on monitor FREEMONITOR-1:\n"),
                               std::string("workspace ID 7 (7) somewhere\n"),
                               std::string("workspace ID 7 (7) on monitor FREEMONITOR-1\n"),
                               std::string("workspace ID 7 (7) on monitor FREEMONITOR-1:\n\twindows: many\n"));
    CAPTURE(text);
    CHECK_FALSE(fm::parseHyprlandWorkspaces(text));
}

TEST_CASE("parseWorkspacesBoundTo finds the rules of one monitor") {
    const std::string rules = ruleFor("1", "eDP-1") + ruleFor("6", "FREEMONITOR-1") + ruleFor("9", "FREEMONITOR-10");
    CHECK(fm::parseWorkspacesBoundTo(rules, "FREEMONITOR-1") == std::vector<std::string>{"6"});
    CHECK(fm::parseWorkspacesBoundTo(rules, "DP-3").empty());
    CHECK(fm::parseWorkspacesBoundTo("", "FREEMONITOR-1").empty());
}

TEST_CASE("a workspace Hyprland put back on the new monitor goes to the focused monitor") {
    FakeCompositor compositor;
    compositor.workspacesReply = LAPTOP_WORKSPACE + workspaceOnTablet(7, 1) + workspaceOnTablet(4, 3);
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    REQUIRE(virtualDisplay.create(TABLET_MODE));
    auto expected = CREATE_REQUESTS;
    expected.emplace_back("workspacerules");
    expected.emplace_back("dispatch moveworkspacetomonitor 7 eDP-1");
    expected.emplace_back("dispatch moveworkspacetomonitor 4 eDP-1");
    CHECK(server.receivedRequests() == expected);
}

TEST_CASE("the focused monitor is chosen when there are several") {
    const std::string external =
            "Monitor DP-3 (ID 2):\n\t1920x1080@60.00000 at -1920x0\n\tscale: 1.00\n\ttransform: 0\n\n";
    FakeCompositor compositor;
    compositor.monitorsReplies = {external + LAPTOP_ONLY, external + TABLET_APPLIED};
    compositor.workspacesReply = workspaceOnTablet(7, 1);
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    REQUIRE(virtualDisplay.create(TABLET_MODE));
    const auto requests = server.receivedRequests();
    CHECK(std::ranges::find(requests, "dispatch moveworkspacetomonitor 7 eDP-1") != requests.end());
}

TEST_CASE("the monitor's own empty workspace stays where it is") {
    FakeCompositor compositor;
    compositor.workspacesReply = LAPTOP_WORKSPACE + workspaceOnTablet(6, 0);
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    REQUIRE(virtualDisplay.create(TABLET_MODE));
    CHECK(server.receivedRequests() == CREATE_REQUESTS);
}

TEST_CASE("a workspace the user bound to the monitor stays on it") {
    FakeCompositor compositor;
    compositor.workspacesReply = workspaceOnTablet(6, 1) + workspaceOnTablet(7, 1);
    compositor.rulesReply = ruleFor("6", "FREEMONITOR-1");
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    REQUIRE(virtualDisplay.create(TABLET_MODE));
    const auto requests = server.receivedRequests();
    CHECK(std::ranges::find(requests, "dispatch moveworkspacetomonitor 6 eDP-1") == requests.end());
    CHECK(std::ranges::find(requests, "dispatch moveworkspacetomonitor 7 eDP-1") != requests.end());
}

TEST_CASE("a special workspace is not moved") {
    FakeCompositor compositor;
    compositor.workspacesReply = "workspace ID -98 (special:scratchpad) on monitor FREEMONITOR-1:\n\twindows: 1\n";
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    REQUIRE(virtualDisplay.create(TABLET_MODE));
    CHECK(server.receivedRequests() == CREATE_REQUESTS);
}

TEST_CASE("a workspace list that cannot be read does not stop the display") {
    FakeCompositor compositor;
    compositor.workspacesReply = "workspace ID x (7) on monitor FREEMONITOR-1:\n";
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    CHECK(virtualDisplay.create(TABLET_MODE));
    CHECK(server.receivedRequests() == CREATE_REQUESTS);
}

TEST_CASE("with no other monitor nothing is moved") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {"", outputAt(2560, 1600, 0, 0)};
    compositor.workspacesReply = workspaceOnTablet(7, 1);
    FakeHyprlandServer server(compositor);
    fm::DisplayMode mode = TABLET_MODE;
    mode.placement = fm::DisplayPlacement::EXPLICIT;
    auto virtualDisplay = makeDisplay(server);

    REQUIRE(virtualDisplay.create(mode));
    const auto requests = server.receivedRequests();
    CHECK(std::ranges::none_of(requests, [](const std::string& request) { return request.starts_with("dispatch "); }));
}

TEST_CASE("a refused monitor rule removes the output again") {
    FakeCompositor compositor;
    compositor.keywordReply = "invalid resolution ";
    FakeHyprlandServer server(compositor);
    {
        auto virtualDisplay = makeDisplay(server);
        const auto target = virtualDisplay.create(TABLET_MODE);
        REQUIRE_FALSE(target);
        CHECK(target.error().code == fm::ErrorCode::DEVICE_FAILURE);
        CHECK(target.error().message.find("invalid resolution") != std::string::npos);
    }
    const auto requests = server.receivedRequests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[4] == "output remove FREEMONITOR-1");
}

TEST_CASE("a refused output creation is reported with Hyprland's reply") {
    FakeCompositor compositor;
    compositor.createReply = "Name already taken";
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    const auto target = virtualDisplay.create(TABLET_MODE);
    REQUIRE_FALSE(target);
    CHECK(target.error().message.find("Name already taken") != std::string::npos);
}

TEST_CASE("an unreadable layout fails before any output is created") {
    FakeCompositor compositor;
    compositor.monitorsReplies = {"Monitor A (ID 0):\n\tgarbage at here\n"};
    FakeHyprlandServer server(compositor);
    auto virtualDisplay = makeDisplay(server);

    REQUIRE_FALSE(virtualDisplay.create(TABLET_MODE));
    CHECK(server.receivedRequests() == std::vector<std::string>{"output remove FREEMONITOR-1", "monitors"});
}

TEST_CASE("create rejects a bad mode and a second create without contacting Hyprland") {
    FakeHyprlandServer server(FakeCompositor{});
    auto virtualDisplay = makeDisplay(server);

    auto badMode = TABLET_MODE;
    badMode.refreshRateHz = 0;
    const auto rejected = virtualDisplay.create(badMode);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    CHECK(server.receivedRequests().empty());

    REQUIRE(virtualDisplay.create(TABLET_MODE));
    const auto second = virtualDisplay.create(TABLET_MODE);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == fm::ErrorCode::INVALID_STATE);
    CHECK(server.receivedRequests().size() == CREATE_REQUESTS.size());
}

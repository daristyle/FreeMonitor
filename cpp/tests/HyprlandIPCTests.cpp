#include "FakeHyprlandServer.hpp"
#include "ScopedEnvironment.hpp"
#include "linux/HyprlandIPC.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("request sends the command and returns the whole reply") {
    FakeHyprlandServer server([](const std::string&) { return std::string("ok"); });
    const fm::HyprlandIPC ipc(server.path());

    const auto reply = ipc.request("output create headless TEST-1");
    REQUIRE(reply);
    CHECK(*reply == "ok");
    CHECK(server.receivedRequests() == std::vector<std::string>{"output create headless TEST-1"});
}

TEST_CASE("request reads a reply that arrives in several pieces") {
    const std::string longReply(20000, 'm');
    FakeHyprlandServer server([&longReply](const std::string&) { return longReply; }, 777);
    const fm::HyprlandIPC ipc(server.path());

    const auto reply = ipc.request("monitors");
    REQUIRE(reply);
    CHECK(*reply == longReply);
}

TEST_CASE("request returns an error reply as text for the caller to judge") {
    FakeHyprlandServer server([](const std::string&) { return std::string("output not found"); });
    const fm::HyprlandIPC ipc(server.path());

    const auto reply = ipc.request("output remove TEST-1");
    REQUIRE(reply);
    CHECK(*reply == "output not found");
}

TEST_CASE("request fails when Hyprland does not answer in time") {
    FakeHyprlandServer server([](const std::string&) { return std::nullopt; });
    const fm::HyprlandIPC ipc(server.path(), 100ms);

    const auto start = std::chrono::steady_clock::now();
    const auto reply = ipc.request("monitors");
    REQUIRE_FALSE(reply);
    CHECK(reply.error().code == fm::ErrorCode::DEVICE_FAILURE);
    CHECK(std::chrono::steady_clock::now() - start < 2s);
}

TEST_CASE("request fails when nothing listens on the socket") {
    std::filesystem::path missing;
    {
        FakeHyprlandServer server([](const std::string&) { return std::string("ok"); });
        missing = server.path();
    }
    const fm::HyprlandIPC ipc(missing);
    const auto reply = ipc.request("monitors");
    REQUIRE_FALSE(reply);
    CHECK_FALSE(reply.error().message.empty());
}

TEST_CASE("request rejects a socket path longer than sockaddr_un allows") {
    const fm::HyprlandIPC ipc(std::filesystem::path("/tmp") / std::string(200, 'p'));
    const auto reply = ipc.request("monitors");
    REQUIRE_FALSE(reply);
    CHECK(reply.error().code == fm::ErrorCode::INVALID_ARGUMENT);
}

TEST_CASE("defaultSocketPath uses XDG_RUNTIME_DIR and the instance signature") {
    SECTION("under Hyprland") {
        const ScopedEnvironment runtime("XDG_RUNTIME_DIR", "/run/user/1000");
        const ScopedEnvironment signature("HYPRLAND_INSTANCE_SIGNATURE", "abc_123");
        const auto path = fm::HyprlandIPC::defaultSocketPath();
        REQUIRE(path);
        CHECK(*path == "/run/user/1000/hypr/abc_123/.socket.sock");
    }
    SECTION("not under Hyprland") {
        const ScopedEnvironment runtime("XDG_RUNTIME_DIR", "/run/user/1000");
        const ScopedEnvironment signature("HYPRLAND_INSTANCE_SIGNATURE", nullptr);
        CHECK_FALSE(fm::HyprlandIPC::defaultSocketPath());
    }
    SECTION("a signature that is not a plain directory name") {
        const ScopedEnvironment runtime("XDG_RUNTIME_DIR", "/run/user/1000");
        const ScopedEnvironment signature("HYPRLAND_INSTANCE_SIGNATURE", "../escape");
        CHECK_FALSE(fm::HyprlandIPC::defaultSocketPath());
    }
    SECTION("no runtime directory") {
        const ScopedEnvironment runtime("XDG_RUNTIME_DIR", nullptr);
        const ScopedEnvironment signature("HYPRLAND_INSTANCE_SIGNATURE", "abc_123");
        CHECK_FALSE(fm::HyprlandIPC::defaultSocketPath());
    }
}

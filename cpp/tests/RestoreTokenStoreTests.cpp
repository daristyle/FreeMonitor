#include "ScopedEnvironment.hpp"
#include "linux/RestoreTokenStore.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
    class TemporaryDirectory {
    private:
        std::filesystem::path directoryPath;

    public:
        TemporaryDirectory()
            : directoryPath(std::filesystem::temp_directory_path() /
                            ("fm-token-test-" + std::to_string(::getpid()) + "-" + std::to_string(std::rand()))) {
            std::filesystem::create_directories(directoryPath);
        }

        ~TemporaryDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(directoryPath, ignored);
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        const std::filesystem::path& path() const {
            return directoryPath;
        }
    };
}

TEST_CASE("load returns nothing when no token was saved") {
    const TemporaryDirectory directory;
    const fm::RestoreTokenStore store(directory.path() / "restore-token");
    CHECK_FALSE(store.load());
}

TEST_CASE("a saved token is loaded back unchanged") {
    const TemporaryDirectory directory;
    const fm::RestoreTokenStore store(directory.path() / "nested" / "dirs" / "restore-token");

    REQUIRE(store.save("d2a5c4f0-7e1b-4f3a-9c2e-1b2c3d4e5f60"));
    const auto token = store.load();
    REQUIRE(token);
    CHECK(*token == "d2a5c4f0-7e1b-4f3a-9c2e-1b2c3d4e5f60");
    CHECK_FALSE(std::filesystem::exists(directory.path() / "nested" / "dirs" / "restore-token.tmp"));
}

TEST_CASE("saving again replaces the previous token") {
    const TemporaryDirectory directory;
    const fm::RestoreTokenStore store(directory.path() / "restore-token");
    REQUIRE(store.save("first"));
    REQUIRE(store.save("second"));
    CHECK(store.load() == "second");
}

TEST_CASE("the token file is readable only by its owner") {
    const TemporaryDirectory directory;
    const fm::RestoreTokenStore store(directory.path() / "restore-token");
    REQUIRE(store.save("secret"));

    const auto permissions = std::filesystem::status(store.path()).permissions();
    CHECK((permissions & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    CHECK((permissions & std::filesystem::perms::others_all) == std::filesystem::perms::none);
}

TEST_CASE("a leftover temporary file from a crashed save is replaced, not reused") {
    const TemporaryDirectory directory;
    const fm::RestoreTokenStore store(directory.path() / "restore-token");
    const auto temporaryPath = directory.path() / "restore-token.tmp";
    std::ofstream(temporaryPath) << "half-written-old-token";
    std::filesystem::permissions(temporaryPath, std::filesystem::perms::all);

    REQUIRE(store.save("fresh"));
    CHECK(store.load() == "fresh");
    CHECK_FALSE(std::filesystem::exists(temporaryPath));
    const auto permissions = std::filesystem::status(store.path()).permissions();
    CHECK((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
}

TEST_CASE("the token file is created private even with a permissive umask") {
    const TemporaryDirectory directory;
    const fm::RestoreTokenStore store(directory.path() / "restore-token");
    const mode_t previousMask = ::umask(0);
    const auto saved = store.save("secret");
    ::umask(previousMask);

    REQUIRE(saved);
    CHECK(std::filesystem::status(store.path()).permissions() ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
}

TEST_CASE("a file with only a line break counts as no token") {
    const TemporaryDirectory directory;
    const fm::RestoreTokenStore store(directory.path() / "restore-token");
    std::ofstream(store.path()) << "\n";
    CHECK_FALSE(store.load());
}

TEST_CASE("save fails when the directory cannot be created") {
    const TemporaryDirectory directory;
    std::ofstream(directory.path() / "not-a-directory") << "x";
    const fm::RestoreTokenStore store(directory.path() / "not-a-directory" / "restore-token");
    CHECK_FALSE(store.save("token"));
}

TEST_CASE("defaultPath follows XDG_STATE_HOME, then HOME") {
    SECTION("XDG_STATE_HOME set") {
        const ScopedEnvironment stateHome("XDG_STATE_HOME", "/tmp/state-home");
        const auto path = fm::RestoreTokenStore::defaultPath("FREEMONITOR-1");
        REQUIRE(path);
        CHECK(*path == "/tmp/state-home/freemonitor/restore-token-FREEMONITOR-1");
    }
    SECTION("only HOME set") {
        const ScopedEnvironment stateHome("XDG_STATE_HOME", nullptr);
        const ScopedEnvironment home("HOME", "/home/someone");
        const auto path = fm::RestoreTokenStore::defaultPath("FREEMONITOR-1");
        REQUIRE(path);
        CHECK(*path == "/home/someone/.local/state/freemonitor/restore-token-FREEMONITOR-1");
    }
    SECTION("relative XDG_STATE_HOME is ignored, as the XDG spec requires") {
        const ScopedEnvironment stateHome("XDG_STATE_HOME", "relative/path");
        const ScopedEnvironment home("HOME", "/home/someone");
        const auto path = fm::RestoreTokenStore::defaultPath("FREEMONITOR-1");
        REQUIRE(path);
        CHECK(*path == "/home/someone/.local/state/freemonitor/restore-token-FREEMONITOR-1");
    }
    SECTION("neither set") {
        const ScopedEnvironment stateHome("XDG_STATE_HOME", nullptr);
        const ScopedEnvironment home("HOME", nullptr);
        CHECK_FALSE(fm::RestoreTokenStore::defaultPath("FREEMONITOR-1"));
    }
}

TEST_CASE("defaultPath keeps one token per output and rejects names that are not plain file names") {
    const ScopedEnvironment stateHome("XDG_STATE_HOME", "/tmp/state-home");
    const auto first = fm::RestoreTokenStore::defaultPath("FREEMONITOR-1");
    const auto second = fm::RestoreTokenStore::defaultPath("eDP_1");
    REQUIRE(first);
    REQUIRE(second);
    CHECK(*first != *second);
    CHECK(*second == "/tmp/state-home/freemonitor/restore-token-eDP_1");

    for (const char* name : {"", ".", "..", "../x", "a/b", "a b", "a,b"}) {
        CAPTURE(name);
        const auto path = fm::RestoreTokenStore::defaultPath(name);
        REQUIRE_FALSE(path);
        CHECK(path.error().code == fm::ErrorCode::INVALID_ARGUMENT);
    }
}

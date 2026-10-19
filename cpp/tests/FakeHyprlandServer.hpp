#pragma once

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

class FakeHyprlandServer {
private:
    static constexpr int POLL_INTERVAL_MS = 10;

    std::filesystem::path directory;
    std::filesystem::path socketPath;
    int listenDescriptor = -1;
    std::function<std::optional<std::string>(const std::string&)> respond;
    std::size_t replyChunkSize;
    std::mutex requestsMutex;
    std::vector<std::string> requests;
    std::atomic<bool> stopping = false;
    std::thread thread;

    bool waitReadable(int descriptor) {
        while (!stopping.load()) {
            pollfd entry{.fd = descriptor, .events = POLLIN, .revents = 0};
            if (::poll(&entry, 1, POLL_INTERVAL_MS) > 0) {
                return true;
            }
        }
        return false;
    }

    void serve(int connection) {
        if (!waitReadable(connection)) {
            return;
        }
        std::array<char, 4096> buffer{};
        const ssize_t received = ::recv(connection, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
            return;
        }
        std::string request(buffer.data(), static_cast<std::size_t>(received));
        {
            const std::lock_guard lock(requestsMutex);
            requests.push_back(request);
        }
        const auto reply = respond(request);
        if (!reply) {
            while (!stopping.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            }
            return;
        }
        for (std::size_t offset = 0; offset < reply->size(); offset += replyChunkSize) {
            const std::size_t length = std::min(replyChunkSize, reply->size() - offset);
            ::send(connection, reply->data() + offset, length, MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void run() {
        while (waitReadable(listenDescriptor)) {
            const int connection = ::accept(listenDescriptor, nullptr, nullptr);
            if (connection < 0) {
                continue;
            }
            serve(connection);
            ::close(connection);
        }
    }

public:
    explicit FakeHyprlandServer(std::function<std::optional<std::string>(const std::string&)> respond,
                                std::size_t replyChunkSize = std::numeric_limits<std::size_t>::max())
        : respond(std::move(respond)), replyChunkSize(replyChunkSize) {
        std::string pattern = (std::filesystem::temp_directory_path() / "fm-hyprland-XXXXXX").string();
        REQUIRE(::mkdtemp(pattern.data()) != nullptr);
        directory = pattern;
        socketPath = directory / ".socket.sock";

        listenDescriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(listenDescriptor >= 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const std::string pathText = socketPath.string();
        REQUIRE(pathText.size() < sizeof(address.sun_path));
        std::memcpy(address.sun_path, pathText.c_str(), pathText.size() + 1);
        REQUIRE(::bind(listenDescriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listenDescriptor, 8) == 0);
        thread = std::thread([this] { run(); });
    }

    ~FakeHyprlandServer() {
        stopping.store(true);
        if (thread.joinable()) {
            thread.join();
        }
        if (listenDescriptor >= 0) {
            ::close(listenDescriptor);
        }
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    FakeHyprlandServer(const FakeHyprlandServer&) = delete;
    FakeHyprlandServer& operator=(const FakeHyprlandServer&) = delete;

    const std::filesystem::path& path() const {
        return socketPath;
    }

    std::vector<std::string> receivedRequests() {
        const std::lock_guard lock(requestsMutex);
        return requests;
    }
};

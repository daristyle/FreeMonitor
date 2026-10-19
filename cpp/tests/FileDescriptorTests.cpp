#include "linux/FileDescriptor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {
    struct Pipe {
        int readEnd = -1;
        int writeEnd = -1;
    };

    Pipe openPipe() {
        std::array<int, 2> descriptors{};
        REQUIRE(::pipe2(descriptors.data(), O_CLOEXEC) == 0);
        return Pipe{descriptors[0], descriptors[1]};
    }

    bool isOpen(int descriptor) {
        return ::fcntl(descriptor, F_GETFD) != -1;
    }

    bool isClosed(int descriptor) {
        return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
    }
}

TEST_CASE("FileDescriptor can be moved but not copied") {
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<fm::FileDescriptor>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<fm::FileDescriptor>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<fm::FileDescriptor>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<fm::FileDescriptor>);
}

TEST_CASE("the destructor closes the descriptor") {
    const Pipe pipe = openPipe();
    {
        const fm::FileDescriptor readEnd(pipe.readEnd);
        CHECK(isOpen(pipe.readEnd));
    }
    CHECK(isClosed(pipe.readEnd));
    ::close(pipe.writeEnd);
}

TEST_CASE("a moved descriptor stays open and the moved-from object closes nothing") {
    const Pipe pipe = openPipe();
    {
        fm::FileDescriptor original(pipe.readEnd);
        {
            const fm::FileDescriptor moved(std::move(original));
            CHECK(moved.get() == pipe.readEnd);
            CHECK(moved.isOpen());
            CHECK_FALSE(original.isOpen());
        }
        CHECK(isClosed(pipe.readEnd));
    }
    ::close(pipe.writeEnd);
}

TEST_CASE("move assignment closes the descriptor it replaces") {
    const Pipe pipe = openPipe();
    {
        fm::FileDescriptor target(pipe.readEnd);
        fm::FileDescriptor source(pipe.writeEnd);

        target = std::move(source);
        CHECK(isClosed(pipe.readEnd));
        CHECK(target.get() == pipe.writeEnd);
        CHECK(isOpen(pipe.writeEnd));
        CHECK_FALSE(source.isOpen());
    }
    CHECK(isClosed(pipe.writeEnd));
}

TEST_CASE("move assignment to itself keeps the descriptor open") {
    const Pipe pipe = openPipe();
    {
        fm::FileDescriptor descriptor(pipe.readEnd);
        fm::FileDescriptor& sameObject = descriptor;
        descriptor = std::move(sameObject);
        CHECK(descriptor.get() == pipe.readEnd);
        CHECK(isOpen(pipe.readEnd));
    }
    CHECK(isClosed(pipe.readEnd));
    ::close(pipe.writeEnd);
}

TEST_CASE("release hands the descriptor over without closing it") {
    const Pipe pipe = openPipe();
    {
        fm::FileDescriptor descriptor(pipe.readEnd);
        CHECK(descriptor.release() == pipe.readEnd);
        CHECK_FALSE(descriptor.isOpen());
    }
    CHECK(isOpen(pipe.readEnd));
    ::close(pipe.readEnd);
    ::close(pipe.writeEnd);
}

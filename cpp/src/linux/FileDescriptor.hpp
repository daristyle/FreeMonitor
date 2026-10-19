#pragma once

#include <unistd.h>
#include <utility>

namespace fm {
    class FileDescriptor {
    private:
        int descriptor;

    public:
        explicit FileDescriptor(int descriptor) : descriptor(descriptor) {}

        ~FileDescriptor() {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
        }

        FileDescriptor(FileDescriptor&& other) noexcept : descriptor(std::exchange(other.descriptor, -1)) {}

        FileDescriptor& operator=(FileDescriptor&& other) noexcept {
            if (this != &other) {
                if (descriptor >= 0) {
                    ::close(descriptor);
                }
                descriptor = std::exchange(other.descriptor, -1);
            }
            return *this;
        }

        FileDescriptor(const FileDescriptor&) = delete;
        FileDescriptor& operator=(const FileDescriptor&) = delete;

        bool isOpen() const {
            return descriptor >= 0;
        }

        int get() const {
            return descriptor;
        }

        int release() {
            return std::exchange(descriptor, -1);
        }
    };
}

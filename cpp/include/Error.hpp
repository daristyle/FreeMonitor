#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace fm {
    enum class ErrorCode : std::uint8_t {
        INVALID_ARGUMENT,
        INVALID_STATE,
        NOT_SUPPORTED,
        PERMISSION_DENIED,
        DEVICE_FAILURE,
    };

    struct Error {
        ErrorCode code;
        std::string message;
    };

    template<typename T = void>
    using Result = std::expected<T, Error>;

}

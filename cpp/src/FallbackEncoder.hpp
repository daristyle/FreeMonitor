#pragma once

#include "Encoder.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fm {
    class FallbackEncoder final : public Encoder {
    private:
        std::vector<std::unique_ptr<Encoder>> candidates;
        Encoder* active = nullptr;
        std::string activeDescription;

    public:
        explicit FallbackEncoder(std::vector<std::unique_ptr<Encoder>> candidates);

        Result<> configure(const EncoderConfig& config) override;
        EncodeStatus encode(const CapturedFrame& input, EncodedFrame& output) noexcept override;
        void requestKeyframe() noexcept override;
        std::string_view description() const noexcept override;
    };

}

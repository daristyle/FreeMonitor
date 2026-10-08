#pragma once

#include "Encoder.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace fm {
    class StubEncoder final : public Encoder {
    private:
        Codec codec;
        EncoderConfig config;
        std::vector<std::uint8_t> outputBuffer;
        std::atomic<bool> keyframeRequested = true;
        bool configured = false;

    public:
        explicit StubEncoder(Codec codec);

        Result<> configure(const EncoderConfig& config) override;
        EncodeStatus encode(const CapturedFrame& input, EncodedFrame& output) noexcept override;
        void requestKeyframe() noexcept override;
    };

}

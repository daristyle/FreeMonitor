#include "FallbackEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    constexpr std::uint32_t SMALL_WIDTH = 64;
    constexpr std::uint32_t LARGE_WIDTH = 4000;
    constexpr std::uint32_t FRAME_HEIGHT = 32;

    class FakeEncoder final : public fm::Encoder {
    private:
        std::string name;
        fm::Codec codec;
        std::uint32_t maxWidth;
        std::array<std::uint8_t, 1> outputBytes{};

    public:
        int keyframeRequests = 0;

        FakeEncoder(std::string name, fm::Codec codec, std::uint32_t maxWidth)
            : name(std::move(name)), codec(codec), maxWidth(maxWidth) {}

        fm::Result<> configure(const fm::EncoderConfig& config) override {
            if (config.width > maxWidth) {
                return std::unexpected(fm::Error{fm::ErrorCode::NOT_SUPPORTED, name + " is too narrow"});
            }
            return {};
        }

        fm::EncodeStatus encode(const fm::CapturedFrame&, fm::EncodedFrame& output) noexcept override {
            output.data = outputBytes;
            output.codec = codec;
            return fm::EncodeStatus::FRAME_ENCODED;
        }

        void requestKeyframe() noexcept override {
            ++keyframeRequests;
        }

        std::string_view description() const noexcept override {
            return name;
        }
    };

    struct Chain {
        FakeEncoder* first;
        FakeEncoder* second;
        std::unique_ptr<fm::FallbackEncoder> encoder;
    };

    std::vector<std::unique_ptr<fm::Encoder>> candidates(std::unique_ptr<FakeEncoder> first,
                                                         std::unique_ptr<FakeEncoder> second) {
        std::vector<std::unique_ptr<fm::Encoder>> list;
        list.push_back(std::move(first));
        list.push_back(std::move(second));
        return list;
    }

    Chain makeChain(std::uint32_t firstMaxWidth, std::uint32_t secondMaxWidth) {
        auto first = std::make_unique<FakeEncoder>("GPU", fm::Codec::H264, firstMaxWidth);
        auto second = std::make_unique<FakeEncoder>("CPU", fm::Codec::JPEG, secondMaxWidth);
        FakeEncoder* firstPointer = first.get();
        FakeEncoder* secondPointer = second.get();
        return Chain{firstPointer, secondPointer,
                     std::make_unique<fm::FallbackEncoder>(candidates(std::move(first), std::move(second)))};
    }

    fm::EncoderConfig configWithWidth(std::uint32_t width) {
        return fm::EncoderConfig{.width = width, .height = FRAME_HEIGHT};
    }

    fm::Codec encodedCodec(fm::Encoder& encoder) {
        fm::EncodedFrame output;
        REQUIRE(encoder.encode(fm::CapturedFrame{}, output) == fm::EncodeStatus::FRAME_ENCODED);
        return output.codec;
    }
}

TEST_CASE("the fallback encoder uses the first encoder that configures") {
    auto chain = makeChain(LARGE_WIDTH, LARGE_WIDTH);
    REQUIRE(chain.encoder->configure(configWithWidth(SMALL_WIDTH)));
    CHECK(encodedCodec(*chain.encoder) == fm::Codec::H264);
    CHECK(chain.encoder->description() == "GPU");
}

TEST_CASE("the fallback encoder skips an encoder that fails and says why") {
    auto chain = makeChain(SMALL_WIDTH, LARGE_WIDTH);
    REQUIRE(chain.encoder->configure(configWithWidth(LARGE_WIDTH)));
    CHECK(encodedCodec(*chain.encoder) == fm::Codec::JPEG);
    CHECK(chain.encoder->description() == "CPU (GPU unavailable: GPU is too narrow)");
}

TEST_CASE("the fallback encoder fails when no encoder works") {
    auto chain = makeChain(SMALL_WIDTH, SMALL_WIDTH);
    const auto configured = chain.encoder->configure(configWithWidth(LARGE_WIDTH));
    REQUIRE_FALSE(configured);
    CHECK(configured.error().code == fm::ErrorCode::NOT_SUPPORTED);
    CHECK(configured.error().message.find("GPU is too narrow") != std::string::npos);
    CHECK(configured.error().message.find("CPU is too narrow") != std::string::npos);

    fm::EncodedFrame output;
    CHECK(chain.encoder->encode(fm::CapturedFrame{}, output) == fm::EncodeStatus::ENCODE_FAILED);
    CHECK(chain.encoder->description().empty());
}

TEST_CASE("the fallback encoder fails to encode before configure") {
    auto chain = makeChain(LARGE_WIDTH, LARGE_WIDTH);
    fm::EncodedFrame output;
    CHECK(chain.encoder->encode(fm::CapturedFrame{}, output) == fm::EncodeStatus::ENCODE_FAILED);
}

TEST_CASE("the fallback encoder fails without candidates") {
    fm::FallbackEncoder encoder({});
    const auto configured = encoder.configure(configWithWidth(SMALL_WIDTH));
    REQUIRE_FALSE(configured);
    CHECK(configured.error().code == fm::ErrorCode::INVALID_STATE);
}

TEST_CASE("a new configure can switch the fallback encoder to another encoder") {
    auto chain = makeChain(SMALL_WIDTH, LARGE_WIDTH);
    REQUIRE(chain.encoder->configure(configWithWidth(SMALL_WIDTH)));
    CHECK(encodedCodec(*chain.encoder) == fm::Codec::H264);
    REQUIRE(chain.encoder->configure(configWithWidth(LARGE_WIDTH)));
    CHECK(encodedCodec(*chain.encoder) == fm::Codec::JPEG);
    REQUIRE(chain.encoder->configure(configWithWidth(SMALL_WIDTH)));
    CHECK(encodedCodec(*chain.encoder) == fm::Codec::H264);
}

TEST_CASE("a keyframe request reaches only the active encoder of the fallback chain") {
    auto chain = makeChain(SMALL_WIDTH, LARGE_WIDTH);
    chain.encoder->requestKeyframe();
    CHECK(chain.first->keyframeRequests == 0);
    CHECK(chain.second->keyframeRequests == 0);

    REQUIRE(chain.encoder->configure(configWithWidth(LARGE_WIDTH)));
    chain.encoder->requestKeyframe();
    CHECK(chain.first->keyframeRequests == 0);
    CHECK(chain.second->keyframeRequests == 1);
}

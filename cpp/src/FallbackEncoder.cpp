#include "FallbackEncoder.hpp"

#include <utility>

namespace fm {
    FallbackEncoder::FallbackEncoder(std::vector<std::unique_ptr<Encoder>> candidates)
        : candidates(std::move(candidates)) {}

    Result<> FallbackEncoder::configure(const EncoderConfig& config) {
        active = nullptr;
        activeDescription.clear();
        std::string skipped;
        for (const auto& candidate : candidates) {
            const auto configured = candidate->configure(config);
            if (configured) {
                active = candidate.get();
                activeDescription = std::string(candidate->description());
                if (!skipped.empty()) {
                    activeDescription += " (" + skipped + ")";
                }
                return {};
            }
            if (!skipped.empty()) {
                skipped += "; ";
            }
            skipped += std::string(candidate->description()) + " unavailable: " + configured.error().message;
        }
        if (skipped.empty()) {
            return std::unexpected(Error{ErrorCode::INVALID_STATE, "There is no encoder to try"});
        }
        return std::unexpected(Error{ErrorCode::NOT_SUPPORTED, "No encoder works: " + skipped});
    }

    EncodeStatus FallbackEncoder::encode(const CapturedFrame& input, EncodedFrame& output) noexcept {
        if (active == nullptr) {
            return EncodeStatus::ENCODE_FAILED;
        }
        return active->encode(input, output);
    }

    void FallbackEncoder::requestKeyframe() noexcept {
        if (active != nullptr) {
            active->requestKeyframe();
        }
    }

    std::string_view FallbackEncoder::description() const noexcept {
        return activeDescription;
    }

}

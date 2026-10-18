#include "JPEGEncoder.hpp"
#include "linux/PipeWireCaptureSource.hpp"
#include "stub/StubEncoder.hpp"
#include "stub/StubVirtualDisplay.hpp"

namespace fm {
    std::unique_ptr<CaptureSource> createCaptureSource() {
        return std::make_unique<PipeWireCaptureSource>();
    }

    std::unique_ptr<Encoder> createEncoder(Codec codec) {
        if (codec == Codec::JPEG) {
            return std::make_unique<JPEGEncoder>();
        }
        return std::make_unique<StubEncoder>(codec);
    }

    std::unique_ptr<VirtualDisplay> createVirtualDisplay() {
        return std::make_unique<StubVirtualDisplay>();
    }
}

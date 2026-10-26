#include "stub/StubCaptureSource.hpp"
#include "stub/StubEncoder.hpp"
#include "stub/StubVirtualDisplay.hpp"

namespace fm {
    std::unique_ptr<CaptureSource> createCaptureSource() {
        return std::make_unique<StubCaptureSource>();
    }

    std::unique_ptr<Encoder> createEncoder(EncoderChoice choice) {
        return std::make_unique<StubEncoder>(choice == EncoderChoice::JPEG ? Codec::JPEG : Codec::H264);
    }

    std::unique_ptr<VirtualDisplay> createVirtualDisplay() {
        return std::make_unique<StubVirtualDisplay>();
    }

}

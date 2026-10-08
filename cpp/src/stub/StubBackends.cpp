#include "stub/StubCaptureSource.hpp"
#include "stub/StubEncoder.hpp"
#include "stub/StubVirtualDisplay.hpp"

namespace fm {
    std::unique_ptr<CaptureSource> createCaptureSource() {
        return std::make_unique<StubCaptureSource>();
    }

    std::unique_ptr<Encoder> createEncoder(Codec codec) {
        return std::make_unique<StubEncoder>(codec);
    }

    std::unique_ptr<VirtualDisplay> createVirtualDisplay() {
        return std::make_unique<StubVirtualDisplay>();
    }

}

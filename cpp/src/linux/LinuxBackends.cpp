#include "JPEGEncoder.hpp"
#include "linux/HyprlandIPC.hpp"
#include "linux/HyprlandVirtualDisplay.hpp"
#include "linux/PipeWireCaptureSource.hpp"
#include "stub/StubEncoder.hpp"
#include "stub/StubVirtualDisplay.hpp"

#include <string>

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
        if (auto socketPath = HyprlandIPC::defaultSocketPath()) {
            return std::make_unique<HyprlandVirtualDisplay>(HyprlandIPC(*socketPath),
                                                            std::string(HyprlandVirtualDisplay::DEFAULT_OUTPUT_NAME));
        }
        return std::make_unique<StubVirtualDisplay>();
    }
}

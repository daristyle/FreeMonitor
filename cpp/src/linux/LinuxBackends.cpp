#include "FallbackEncoder.hpp"
#include "JPEGEncoder.hpp"
#include "NVENCEncoder.hpp"
#include "linux/HyprlandIPC.hpp"
#include "linux/HyprlandVirtualDisplay.hpp"
#include "linux/PipeWireCaptureSource.hpp"
#include "linux/VAAPIDevices.hpp"
#include "linux/VAAPIEncoder.hpp"
#include "stub/StubVirtualDisplay.hpp"

#include <string>
#include <utility>
#include <vector>

namespace fm {
    std::unique_ptr<CaptureSource> createCaptureSource() {
        return std::make_unique<PipeWireCaptureSource>();
    }

    std::unique_ptr<Encoder> createEncoder(EncoderChoice choice) {
        switch (choice) {
            case EncoderChoice::NVENC:
                return std::make_unique<NVENCEncoder>();
            case EncoderChoice::VAAPI: {
                const auto devices = findVAAPIDevices();
                return std::make_unique<VAAPIEncoder>(devices.empty() ? VAAPIDevice{} : devices.front());
            }
            case EncoderChoice::JPEG:
                return std::make_unique<JPEGEncoder>();
            case EncoderChoice::AUTO:
                break;
        }
        std::vector<std::unique_ptr<Encoder>> candidates;
        candidates.push_back(std::make_unique<NVENCEncoder>());
        for (VAAPIDevice& device : findVAAPIDevices()) {
            candidates.push_back(std::make_unique<VAAPIEncoder>(std::move(device)));
        }
        candidates.push_back(std::make_unique<JPEGEncoder>());
        return std::make_unique<FallbackEncoder>(std::move(candidates));
    }

    std::unique_ptr<VirtualDisplay> createVirtualDisplay() {
        if (auto socketPath = HyprlandIPC::defaultSocketPath()) {
            return std::make_unique<HyprlandVirtualDisplay>(HyprlandIPC(*socketPath),
                                                            std::string(HyprlandVirtualDisplay::DEFAULT_OUTPUT_NAME));
        }
        return std::make_unique<StubVirtualDisplay>();
    }
}

#include "CaptureSource.hpp"
#include "Encoder.hpp"
#include "VirtualDisplay.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <print>

namespace {
    constexpr int FRAME_COUNT = 60;
    constexpr std::chrono::milliseconds CAPTURE_TIMEOUT{100};
}

int main() {
    const fm::DisplayMode displayMode{
            .width = 1920, .height = 1080, .refreshRateHz = 60, .positionX = 1920, .positionY = 0};

    auto virtualDisplay = fm::createVirtualDisplay();
    auto captureTarget = virtualDisplay->create(displayMode);
    if (!captureTarget) {
        std::println(stderr, "Could not create the virtual display: {}", captureTarget.error().message);
        return 1;
    }

    auto captureSource = fm::createCaptureSource();
    if (auto started = captureSource->start(*captureTarget); !started) {
        std::println(stderr, "Could not start capture: {}", started.error().message);
        return 1;
    }

    auto encoder = fm::createEncoder(fm::Codec::JPEG);
    const fm::EncoderConfig encoderConfig{
            .width = displayMode.width, .height = displayMode.height, .framesPerSecond = displayMode.refreshRateHz};
    if (auto configured = encoder->configure(encoderConfig); !configured) {
        std::println(stderr, "Could not configure the encoder: {}", configured.error().message);
        return 1;
    }

    int encodedFrames = 0;
    int droppedFrames = 0;
    std::size_t encodedBytes = 0;
    fm::CapturedFrame capturedFrame;
    fm::EncodedFrame encodedFrame;
    for (int i = 0; i < FRAME_COUNT; ++i) {
        const auto captureStatus = captureSource->acquireFrame(capturedFrame, CAPTURE_TIMEOUT);
        if (captureStatus == fm::CaptureStatus::CAPTURE_LOST) {
            std::println(stderr, "Capture lost");
            return 1;
        }
        if (captureStatus == fm::CaptureStatus::NO_NEW_FRAME) {
            continue;
        }

        if (encoder->encode(capturedFrame, encodedFrame) == fm::EncodeStatus::FRAME_ENCODED) {
            ++encodedFrames;
            encodedBytes += encodedFrame.data.size();
        } else {
            ++droppedFrames;
        }
        captureSource->releaseFrame();
    }

    captureSource->stop();
    std::println("Captured on {}: {} frames encoded, {} dropped, {} bytes", captureTarget->outputName, encodedFrames,
                 droppedFrames, encodedBytes);
    return 0;
}

#include "EndpointParsing.hpp"
#include "FrameReceiver.hpp"
#include "Packets.hpp"
#include "TabletSession.hpp"
#include "Timestamp.hpp"
#include "UDPSocket.hpp"

#include <jni.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fm {
    namespace {
        constexpr const char* NATIVE_BRIDGE_CLASS = "io/github/daristyle/freemonitor/NativeBridge";
        constexpr const char* ILLEGAL_STATE_EXCEPTION = "java/lang/IllegalStateException";
        constexpr std::uint32_t DEVELOPMENT_SESSION_ID = 0;
        constexpr jint MAX_PORT = 65535;
        constexpr jint MAX_SCREEN_VALUE = 65535;
        constexpr jsize STATISTICS_COUNT = 8;
        constexpr jsize FRAME_INFO_COUNT = 7;
        constexpr jsize SESSION_STATUS_COUNT = 9;
        constexpr jlong UNKNOWN_ROUND_TRIP = -1;
        constexpr jlong NO_CURSOR_SHAPE = -1;
        constexpr unsigned SHAPE_HEIGHT_SHIFT = 16;
        constexpr unsigned SHAPE_HOTSPOT_X_SHIFT = 32;
        constexpr unsigned SHAPE_HOTSPOT_Y_SHIFT = 48;

        FrameReceiver* toReceiver(jlong handle) {
            return reinterpret_cast<FrameReceiver*>(static_cast<std::intptr_t>(handle));
        }

        jlong toHandle(FrameReceiver* receiver) {
            return static_cast<jlong>(reinterpret_cast<std::intptr_t>(receiver));
        }

        void throwIllegalState(JNIEnv* environment, const std::string& message) {
            jclass exceptionClass = environment->FindClass(ILLEGAL_STATE_EXCEPTION);
            if (exceptionClass != nullptr) {
                environment->ThrowNew(exceptionClass, message.c_str());
                environment->DeleteLocalRef(exceptionClass);
            }
        }

        jint protocolVersion(JNIEnv*, jclass) {
            return PROTOCOL_VERSION;
        }

        jint maxFrameSize(JNIEnv*, jclass) {
            return static_cast<jint>(MAX_FRAME_SIZE);
        }

        jlong createReceiver(JNIEnv* environment, jclass, jint port) {
            if (port < 0 || port > MAX_PORT) {
                throwIllegalState(environment, "Port must be 0 to 65535");
                return 0;
            }
            auto receiver = FrameReceiver::open(Endpoint{ANY_IP_ADDRESS, static_cast<std::uint16_t>(port)},
                                                DEVELOPMENT_SESSION_ID);
            if (!receiver) {
                throwIllegalState(environment, receiver.error().message);
                return 0;
            }
            (*receiver)->start();
            return toHandle(receiver->release());
        }

        bool isScreenValue(jint value) {
            return value > 0 && value <= MAX_SCREEN_VALUE;
        }

        jlong createSessionReceiver(JNIEnv* environment, jclass, jstring host, jint width, jint height,
                                    jint refreshHz) {
            if (!isScreenValue(width) || !isScreenValue(height) || !isScreenValue(refreshHz)) {
                throwIllegalState(environment, "The screen size and refresh rate must be 1 to 65535");
                return 0;
            }
            const char* hostCharacters = environment->GetStringUTFChars(host, nullptr);
            if (hostCharacters == nullptr) {
                return 0;
            }
            const std::string hostText(hostCharacters);
            environment->ReleaseStringUTFChars(host, hostCharacters);
            const auto endpoint = parseEndpoint(hostText);
            if (!endpoint) {
                throwIllegalState(environment, "The host must be IPv4:port, for example 192.168.1.10:50500");
                return 0;
            }
            const TabletSettings settings{.host = *endpoint,
                                          .width = static_cast<std::uint16_t>(width),
                                          .height = static_cast<std::uint16_t>(height),
                                          .refreshHz = static_cast<std::uint16_t>(refreshHz),
                                          .features = FeatureFlags::FEC | FeatureFlags::CURSOR_OVERLAY};
            auto receiver = FrameReceiver::openSession(settings);
            if (!receiver) {
                throwIllegalState(environment, receiver.error().message);
                return 0;
            }
            (*receiver)->start();
            return toHandle(receiver->release());
        }

        jint nextFrame(JNIEnv* environment, jclass, jlong handle, jbyteArray destination, jlongArray frameInfo,
                       jint timeoutMs) {
            if (environment->GetArrayLength(frameInfo) < FRAME_INFO_COUNT) {
                throwIllegalState(environment, "The frame info array is too short");
                return 0;
            }
            FrameReceiver* receiver = toReceiver(handle);
            ReassembledFrame frame;
            if (receiver->acquire(frame, std::chrono::milliseconds(timeoutMs)) != MailboxStatus::FRAME_READY) {
                return 0;
            }
            const std::array<jlong, FRAME_INFO_COUNT> info{
                    static_cast<jlong>(frame.frameId),
                    static_cast<jlong>(frame.data.size()),
                    frame.keyframe ? 1 : 0,
                    static_cast<jlong>(frame.captureTimestampUs),
                    static_cast<jlong>(toTimestampUs(frame.firstFragmentTime)),
                    static_cast<jlong>(toTimestampUs(frame.completeTime)),
                    static_cast<jlong>(frame.codec),
            };
            environment->SetLongArrayRegion(frameInfo, 0, FRAME_INFO_COUNT, info.data());
            const auto length = static_cast<jsize>(frame.data.size());
            const bool fits = length <= environment->GetArrayLength(destination);
            if (fits) {
                environment->SetByteArrayRegion(destination, 0, length,
                                                reinterpret_cast<const jbyte*>(frame.data.data()));
            }
            receiver->release();
            if (!fits) {
                throwIllegalState(environment, "The frame buffer is smaller than the frame");
                return 0;
            }
            return length;
        }

        jlongArray receiverStatistics(JNIEnv* environment, jclass, jlong handle) {
            const FrameReceiverStatistics statistics = toReceiver(handle)->statistics();
            const std::array<jlong, STATISTICS_COUNT> values{
                    static_cast<jlong>(statistics.packets),        static_cast<jlong>(statistics.invalidPackets),
                    static_cast<jlong>(statistics.ignoredPackets), static_cast<jlong>(statistics.framesCompleted),
                    static_cast<jlong>(statistics.framesDropped),  static_cast<jlong>(statistics.framesReplaced),
                    static_cast<jlong>(statistics.senderRestarts), static_cast<jlong>(statistics.framesRepaired),
            };
            jlongArray array = environment->NewLongArray(STATISTICS_COUNT);
            if (array != nullptr) {
                environment->SetLongArrayRegion(array, 0, STATISTICS_COUNT, values.data());
            }
            return array;
        }

        void requestKeyframe(JNIEnv*, jclass, jlong handle) {
            toReceiver(handle)->requestKeyframe();
        }

        void reconnect(JNIEnv*, jclass, jlong handle) {
            toReceiver(handle)->reconnect();
        }

        jlongArray sessionStatus(JNIEnv* environment, jclass, jlong handle) {
            const FrameReceiverSessionStatus status = toReceiver(handle)->sessionStatus();
            const std::array<jlong, SESSION_STATUS_COUNT> values{
                    static_cast<jlong>(status.state),
                    static_cast<jlong>(status.rejectReason),
                    static_cast<jlong>(status.sessionId),
                    status.roundTripTime ? static_cast<jlong>(status.roundTripTime->count()) : UNKNOWN_ROUND_TRIP,
                    static_cast<jlong>(status.stream.codec),
                    static_cast<jlong>(status.stream.width),
                    static_cast<jlong>(status.stream.height),
                    static_cast<jlong>(status.stream.fps),
                    static_cast<jlong>(status.sessionCount),
            };
            jlongArray array = environment->NewLongArray(SESSION_STATUS_COUNT);
            if (array != nullptr) {
                environment->SetLongArrayRegion(array, 0, SESSION_STATUS_COUNT, values.data());
            }
            return array;
        }

        jlong cursorState(JNIEnv*, jclass, jlong handle) {
            return static_cast<jlong>(toReceiver(handle)->cursorSnapshot());
        }

        jlong cursorShape(JNIEnv* environment, jclass, jlong handle, jlong state, jintArray pixels) {
            const jsize length = environment->GetArrayLength(pixels);
            auto* elements = static_cast<jint*>(environment->GetPrimitiveArrayCritical(pixels, nullptr));
            if (elements == nullptr) {
                return NO_CURSOR_SHAPE;
            }
            const auto info = toReceiver(handle)->copyCursorShape(
                    static_cast<std::uint64_t>(state),
                    std::span(reinterpret_cast<std::uint32_t*>(elements), static_cast<std::size_t>(length)));
            environment->ReleasePrimitiveArrayCritical(pixels, elements, info ? 0 : JNI_ABORT);
            if (!info) {
                return NO_CURSOR_SHAPE;
            }
            return static_cast<jlong>(std::uint64_t{info->width} | (std::uint64_t{info->height} << SHAPE_HEIGHT_SHIFT) |
                                      (std::uint64_t{info->hotspotX} << SHAPE_HOTSPOT_X_SHIFT) |
                                      (std::uint64_t{info->hotspotY} << SHAPE_HOTSPOT_Y_SHIFT));
        }

        void destroyReceiver(JNIEnv*, jclass, jlong handle) {
            delete toReceiver(handle);
        }

        const std::array<JNINativeMethod, 12> NATIVE_METHODS{{
                {"protocolVersion", "()I", reinterpret_cast<void*>(&protocolVersion)},
                {"maxFrameSize", "()I", reinterpret_cast<void*>(&maxFrameSize)},
                {"createReceiver", "(I)J", reinterpret_cast<void*>(&createReceiver)},
                {"createSessionReceiver", "(Ljava/lang/String;III)J", reinterpret_cast<void*>(&createSessionReceiver)},
                {"nextFrame", "(J[B[JI)I", reinterpret_cast<void*>(&nextFrame)},
                {"receiverStatistics", "(J)[J", reinterpret_cast<void*>(&receiverStatistics)},
                {"requestKeyframe", "(J)V", reinterpret_cast<void*>(&requestKeyframe)},
                {"reconnect", "(J)V", reinterpret_cast<void*>(&reconnect)},
                {"sessionStatus", "(J)[J", reinterpret_cast<void*>(&sessionStatus)},
                {"cursorState", "(J)J", reinterpret_cast<void*>(&cursorState)},
                {"cursorShape", "(JJ[I)J", reinterpret_cast<void*>(&cursorShape)},
                {"destroyReceiver", "(J)V", reinterpret_cast<void*>(&destroyReceiver)},
        }};
    }
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* virtualMachine, void*) {
    JNIEnv* environment = nullptr;
    if (virtualMachine->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass nativeBridge = environment->FindClass(fm::NATIVE_BRIDGE_CLASS);
    if (nativeBridge == nullptr) {
        return JNI_ERR;
    }
    const jint registered = environment->RegisterNatives(nativeBridge, fm::NATIVE_METHODS.data(),
                                                         static_cast<jint>(fm::NATIVE_METHODS.size()));
    environment->DeleteLocalRef(nativeBridge);
    return registered == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}

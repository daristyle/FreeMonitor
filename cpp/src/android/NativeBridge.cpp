#include "FrameReceiver.hpp"
#include "Packets.hpp"
#include "Timestamp.hpp"
#include "UDPSocket.hpp"

#include <jni.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fm {
    namespace {
        constexpr const char* NATIVE_BRIDGE_CLASS = "io/github/daristyle/freemonitor/NativeBridge";
        constexpr const char* ILLEGAL_STATE_EXCEPTION = "java/lang/IllegalStateException";
        constexpr std::uint32_t DEVELOPMENT_SESSION_ID = 0;
        constexpr jint MAX_PORT = 65535;
        constexpr jint NOT_JPEG = -1;
        constexpr jsize STATISTICS_COUNT = 7;
        constexpr jsize FRAME_INFO_COUNT = 6;

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
            };
            environment->SetLongArrayRegion(frameInfo, 0, FRAME_INFO_COUNT, info.data());
            jint result = NOT_JPEG;
            bool destinationTooSmall = false;
            if (frame.codec == Codec::JPEG) {
                const auto length = static_cast<jsize>(frame.data.size());
                if (length <= environment->GetArrayLength(destination)) {
                    environment->SetByteArrayRegion(destination, 0, length,
                                                    reinterpret_cast<const jbyte*>(frame.data.data()));
                    result = length;
                } else {
                    destinationTooSmall = true;
                }
            }
            receiver->release();
            if (destinationTooSmall) {
                throwIllegalState(environment, "The frame buffer is smaller than the frame");
                return 0;
            }
            return result;
        }

        jlongArray receiverStatistics(JNIEnv* environment, jclass, jlong handle) {
            const FrameReceiverStatistics statistics = toReceiver(handle)->statistics();
            const std::array<jlong, STATISTICS_COUNT> values{
                    static_cast<jlong>(statistics.packets),        static_cast<jlong>(statistics.invalidPackets),
                    static_cast<jlong>(statistics.ignoredPackets), static_cast<jlong>(statistics.framesCompleted),
                    static_cast<jlong>(statistics.framesDropped),  static_cast<jlong>(statistics.framesReplaced),
                    static_cast<jlong>(statistics.senderRestarts),
            };
            jlongArray array = environment->NewLongArray(STATISTICS_COUNT);
            if (array != nullptr) {
                environment->SetLongArrayRegion(array, 0, STATISTICS_COUNT, values.data());
            }
            return array;
        }

        void destroyReceiver(JNIEnv*, jclass, jlong handle) {
            delete toReceiver(handle);
        }

        const std::array<JNINativeMethod, 6> NATIVE_METHODS{{
                {"protocolVersion", "()I", reinterpret_cast<void*>(&protocolVersion)},
                {"maxFrameSize", "()I", reinterpret_cast<void*>(&maxFrameSize)},
                {"createReceiver", "(I)J", reinterpret_cast<void*>(&createReceiver)},
                {"nextFrame", "(J[B[JI)I", reinterpret_cast<void*>(&nextFrame)},
                {"receiverStatistics", "(J)[J", reinterpret_cast<void*>(&receiverStatistics)},
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

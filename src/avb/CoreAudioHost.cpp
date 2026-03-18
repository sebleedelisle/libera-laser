#include "AvbAudioHost.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cctype>
#include <memory>
#include <mutex>
#include <unordered_map>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#if defined(_WIN32) && __has_include(<RtAudio.h>)
#include <RtAudio.h>
#define LIBERA_HAS_RTAUDIO_AUDIO_HOST 1
#endif

namespace libera::avb::detail {

#if defined(__APPLE__)
namespace {

constexpr std::uint32_t queueBufferCount = 3;
constexpr std::uint32_t framesPerBuffer = 512;

std::string cfStringToStdString(CFStringRef value) {
    if (value == nullptr) {
        return {};
    }

    const auto length = CFStringGetLength(value);
    const auto maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result;
    result.resize(static_cast<std::size_t>(maxSize));

    if (!CFStringGetCString(value, result.data(), static_cast<CFIndex>(result.size()), kCFStringEncodingUTF8)) {
        return {};
    }

    result.resize(std::strlen(result.c_str()));
    return result;
}

std::string copyStringProperty(AudioObjectID objectId,
                               AudioObjectPropertySelector selector,
                               AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    AudioObjectPropertyAddress address{
        selector,
        scope,
        kAudioObjectPropertyElementMain};

    if (!AudioObjectHasProperty(objectId, &address)) {
        return {};
    }

    CFStringRef value = nullptr;
    UInt32 dataSize = sizeof(value);
    const auto status = AudioObjectGetPropertyData(objectId, &address, 0, nullptr, &dataSize, &value);
    if (status != noErr || value == nullptr) {
        return {};
    }

    std::string result = cfStringToStdString(value);
    CFRelease(value);
    return result;
}

std::uint32_t readOutputChannelCount(AudioDeviceID deviceId) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyStreamConfiguration,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain};

    if (!AudioObjectHasProperty(deviceId, &address)) {
        return 0;
    }

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(deviceId, &address, 0, nullptr, &dataSize) != noErr ||
        dataSize == 0) {
        return 0;
    }

    auto bytes = std::make_unique<std::uint8_t[]>(dataSize);
    auto* bufferList = reinterpret_cast<AudioBufferList*>(bytes.get());
    if (AudioObjectGetPropertyData(deviceId, &address, 0, nullptr, &dataSize, bufferList) != noErr) {
        return 0;
    }

    std::uint32_t channelCount = 0;
    for (UInt32 index = 0; index < bufferList->mNumberBuffers; ++index) {
        channelCount += bufferList->mBuffers[index].mNumberChannels;
    }
    return channelCount;
}

std::uint32_t readNominalPointRate(AudioDeviceID deviceId) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};

    if (!AudioObjectHasProperty(deviceId, &address)) {
        return 0;
    }

    Float64 sampleRate = 0.0;
    UInt32 dataSize = sizeof(sampleRate);
    if (AudioObjectGetPropertyData(deviceId, &address, 0, nullptr, &dataSize, &sampleRate) != noErr ||
        sampleRate <= 0.0) {
        return 0;
    }

    return static_cast<std::uint32_t>(std::llround(sampleRate));
}

std::vector<std::uint32_t> readSupportedPointRates(AudioDeviceID deviceId,
                                                   std::uint32_t fallbackRate) {
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};

    std::vector<std::uint32_t> supportedRates;
    if (!AudioObjectHasProperty(deviceId, &address)) {
        if (fallbackRate > 0) {
            supportedRates.push_back(fallbackRate);
        }
        return supportedRates;
    }

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(deviceId, &address, 0, nullptr, &dataSize) != noErr ||
        dataSize == 0) {
        if (fallbackRate > 0) {
            supportedRates.push_back(fallbackRate);
        }
        return supportedRates;
    }

    std::vector<AudioValueRange> ranges(dataSize / sizeof(AudioValueRange));
    if (AudioObjectGetPropertyData(deviceId, &address, 0, nullptr, &dataSize, ranges.data()) != noErr) {
        if (fallbackRate > 0) {
            supportedRates.push_back(fallbackRate);
        }
        return supportedRates;
    }

    const std::vector<std::uint32_t> commonRates{
        32000, 44100, 48000, 88200, 96000, 176400, 192000};

    for (const auto& range : ranges) {
        const auto minimum = static_cast<std::uint32_t>(std::llround(range.mMinimum));
        const auto maximum = static_cast<std::uint32_t>(std::llround(range.mMaximum));

        if (minimum == maximum) {
            supportedRates.push_back(minimum);
            continue;
        }

        for (const auto candidate : commonRates) {
            if (candidate >= minimum && candidate <= maximum) {
                supportedRates.push_back(candidate);
            }
        }
    }

    if (supportedRates.empty() && fallbackRate > 0) {
        supportedRates.push_back(fallbackRate);
    }

    std::sort(supportedRates.begin(), supportedRates.end());
    supportedRates.erase(std::unique(supportedRates.begin(), supportedRates.end()), supportedRates.end());
    return supportedRates;
}

std::vector<AudioDeviceID> readAllAudioDeviceIds() {
    AudioObjectPropertyAddress address{
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};

    UInt32 dataSize = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &dataSize) != noErr ||
        dataSize == 0) {
        return {};
    }

    std::vector<AudioDeviceID> deviceIds(dataSize / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                   &address,
                                   0,
                                   nullptr,
                                   &dataSize,
                                   deviceIds.data()) != noErr) {
        return {};
    }
    return deviceIds;
}

class CoreAudioQueueStream final : public AudioOutputStream {
public:
    CoreAudioQueueStream(std::string deviceUid,
                         std::uint32_t pointRateValue,
                         std::uint32_t channelCountValue,
                         AudioOutputCallback callback)
    : deviceUidValue(std::move(deviceUid))
    , pointRateValue(pointRateValue)
    , channelCountValue(channelCountValue)
    , callbackValue(std::move(callback)) {}

    ~CoreAudioQueueStream() override {
        stop();
    }

    bool start() override {
        if (running.load(std::memory_order_relaxed)) {
            return true;
        }

        AudioStreamBasicDescription streamFormat{};
        streamFormat.mSampleRate = static_cast<Float64>(pointRateValue);
        streamFormat.mFormatID = kAudioFormatLinearPCM;
        streamFormat.mFormatFlags = kLinearPCMFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        streamFormat.mBytesPerPacket = channelCountValue * sizeof(Float32);
        streamFormat.mFramesPerPacket = 1;
        streamFormat.mBytesPerFrame = channelCountValue * sizeof(Float32);
        streamFormat.mChannelsPerFrame = channelCountValue;
        streamFormat.mBitsPerChannel = 32;

        const auto createStatus = AudioQueueNewOutput(
            &streamFormat,
            &CoreAudioQueueStream::audioQueueCallback,
            this,
            nullptr,
            nullptr,
            0,
            &queue);
        if (createStatus != noErr || queue == nullptr) {
            return false;
        }

        CFStringRef deviceUidRef =
            CFStringCreateWithCString(kCFAllocatorDefault,
                                      deviceUidValue.c_str(),
                                      kCFStringEncodingUTF8);
        if (deviceUidRef == nullptr) {
            stop();
            return false;
        }

        const auto propertyStatus = AudioQueueSetProperty(
            queue,
            kAudioQueueProperty_CurrentDevice,
            &deviceUidRef,
            static_cast<UInt32>(sizeof(deviceUidRef)));
        CFRelease(deviceUidRef);

        if (propertyStatus != noErr) {
            stop();
            return false;
        }

        const UInt32 bufferByteSize =
            framesPerBuffer * channelCountValue * static_cast<UInt32>(sizeof(Float32));
        for (std::uint32_t index = 0; index < queueBufferCount; ++index) {
            AudioQueueBufferRef buffer = nullptr;
            if (AudioQueueAllocateBuffer(queue, bufferByteSize, &buffer) != noErr || buffer == nullptr) {
                stop();
                return false;
            }
            buffers[index] = buffer;
        }

        running.store(true, std::memory_order_relaxed);
        for (auto* buffer : buffers) {
            if (!refillAndEnqueueBuffer(buffer)) {
                stop();
                return false;
            }
        }

        if (AudioQueueStart(queue, nullptr) != noErr) {
            stop();
            return false;
        }

        return true;
    }

    void stop() override {
        const bool wasRunning = running.exchange(false, std::memory_order_relaxed);
        if (queue != nullptr) {
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
            queue = nullptr;
        }

        if (wasRunning) {
            for (auto& buffer : buffers) {
                buffer = nullptr;
            }
        }
    }

    std::uint32_t pointRate() const override {
        return pointRateValue;
    }

    std::uint32_t channelCount() const override {
        return channelCountValue;
    }

private:
    static void audioQueueCallback(void* userData,
                                   AudioQueueRef queueRef,
                                   AudioQueueBufferRef buffer) {
        (void)queueRef;
        auto* self = static_cast<CoreAudioQueueStream*>(userData);
        if (!self) {
            return;
        }
        self->refillAndEnqueueBuffer(buffer);
    }

    bool refillAndEnqueueBuffer(AudioQueueBufferRef buffer) {
        if (!running.load(std::memory_order_relaxed) || queue == nullptr || buffer == nullptr) {
            return false;
        }

        const auto sampleCount = static_cast<std::size_t>(framesPerBuffer) * channelCountValue;
        auto* samples = reinterpret_cast<Float32*>(buffer->mAudioData);
        if (samples == nullptr) {
            return false;
        }

        std::fill_n(samples, sampleCount, 0.0f);

        if (callbackValue) {
            const auto leadFrames =
                static_cast<std::uint64_t>(queueBufferCount - 1) * framesPerBuffer;
            const auto leadTime = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(
                    static_cast<double>(leadFrames) / static_cast<double>(pointRateValue)));

            callbackValue(samples, framesPerBuffer, channelCountValue, leadTime);
        }

        buffer->mAudioDataByteSize =
            framesPerBuffer * channelCountValue * static_cast<UInt32>(sizeof(Float32));
        return AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr) == noErr;
    }

    std::string deviceUidValue;
    std::uint32_t pointRateValue = 0;
    std::uint32_t channelCountValue = 0;
    AudioOutputCallback callbackValue;
    AudioQueueRef queue = nullptr;
    std::array<AudioQueueBufferRef, queueBufferCount> buffers{};
    std::atomic<bool> running{false};
};

class CoreAudioHost final : public AudioHost {
public:
    std::vector<AudioOutputDeviceInfo> listOutputDevices() override {
        std::vector<AudioOutputDeviceInfo> devices;

        for (const auto deviceId : readAllAudioDeviceIds()) {
            const auto uid = copyStringProperty(deviceId, kAudioDevicePropertyDeviceUID);
            const auto label = copyStringProperty(deviceId, kAudioObjectPropertyName);
            const auto outputChannels = readOutputChannelCount(deviceId);
            if (uid.empty() || label.empty() || outputChannels == 0) {
                continue;
            }

            const auto defaultPointRate = readNominalPointRate(deviceId);
            const auto supportedPointRates =
                readSupportedPointRates(deviceId, defaultPointRate);

            AudioOutputDeviceInfo deviceInfo;
            deviceInfo.backendDeviceId = static_cast<std::uint32_t>(deviceId);
            deviceInfo.uid = uid;
            deviceInfo.label = label;
            deviceInfo.outputChannels = outputChannels;
            deviceInfo.defaultPointRate = defaultPointRate;
            deviceInfo.pointRateMutable = supportedPointRates.size() > 1;
            deviceInfo.supportedPointRates = supportedPointRates;
            devices.push_back(std::move(deviceInfo));
        }

        return devices;
    }

    std::unique_ptr<AudioOutputStream> openOutputStream(
        const AudioOutputDeviceInfo& device,
        std::uint32_t pointRateValue,
        AudioOutputCallback callback) override {
        if (device.uid.empty() || device.outputChannels == 0 || pointRateValue == 0) {
            return nullptr;
        }

        return std::make_unique<CoreAudioQueueStream>(
            device.uid,
            pointRateValue,
            device.outputChannels,
            std::move(callback));
    }
};

} // namespace
#endif

#if defined(_WIN32) && defined(LIBERA_HAS_RTAUDIO_AUDIO_HOST)
namespace {

constexpr std::uint32_t rtAudioFramesPerBuffer = 512;
constexpr std::uint32_t rtAudioBufferCount = 4;

std::vector<std::uint32_t> normalizeRtAudioPointRates(
    const std::vector<unsigned int>& sampleRates,
    std::uint32_t fallbackRate) {
    std::vector<std::uint32_t> pointRates;
    pointRates.reserve(sampleRates.size() + (fallbackRate > 0 ? 1u : 0u));
    for (const auto sampleRate : sampleRates) {
        if (sampleRate > 0) {
            pointRates.push_back(sampleRate);
        }
    }
    if (fallbackRate > 0) {
        pointRates.push_back(fallbackRate);
    }

    std::sort(pointRates.begin(), pointRates.end());
    pointRates.erase(std::unique(pointRates.begin(), pointRates.end()), pointRates.end());
    return pointRates;
}

std::string makeRtAudioDeviceUid(const std::string& baseName, std::uint32_t duplicateIndex) {
    if (duplicateIndex <= 1) {
        return "asio::" + baseName;
    }
    return "asio::" + baseName + "#" + std::to_string(duplicateIndex);
}

std::string makeRtAudioDeviceLabel(const std::string& baseName, std::uint32_t duplicateIndex) {
    if (duplicateIndex <= 1) {
        return baseName;
    }
    return baseName + " (" + std::to_string(duplicateIndex) + ")";
}

bool hasCompiledAsioApi() {
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    return std::find(apis.begin(), apis.end(), RtAudio::WINDOWS_ASIO) != apis.end();
}

class RtAudioStream final : public AudioOutputStream {
public:
    RtAudioStream(std::uint32_t deviceId,
                  std::uint32_t pointRateValue,
                  std::uint32_t channelCountValue,
                  AudioOutputCallback callback)
    : deviceIdValue(deviceId)
    , requestedPointRateValue(pointRateValue)
    , channelCountValue(channelCountValue)
    , callbackValue(std::move(callback)) {}

    ~RtAudioStream() override {
        stop();
    }

    bool start() override {
        if (running.load(std::memory_order_relaxed)) {
            return true;
        }
        if (requestedPointRateValue == 0 || channelCountValue == 0 || !hasCompiledAsioApi()) {
            return false;
        }

        {
            std::lock_guard lock(globalAsioStreamMutex());
            // ASIO only allows one driver instance per process. Mirror that
            // limitation here so AVB startup fails predictably instead of
            // fighting the driver with multiple independent opens.
            if (activeAsioStreamCount() > 0) {
                return false;
            }
            activeAsioStreamCount() += 1;
            countedAsioStream = true;
        }

        try {
            audio = std::make_unique<RtAudio>(RtAudio::WINDOWS_ASIO);
            audio->showWarnings(false);

            RtAudio::StreamParameters outputParameters;
            outputParameters.deviceId = deviceIdValue;
            outputParameters.nChannels = channelCountValue;
            outputParameters.firstChannel = 0;

            RtAudio::StreamOptions options;
            options.flags = RTAUDIO_MINIMIZE_LATENCY;
            options.numberOfBuffers = rtAudioBufferCount;

            unsigned int bufferFrames = rtAudioFramesPerBuffer;
            audio->openStream(&outputParameters,
                              nullptr,
                              RTAUDIO_FLOAT32,
                              requestedPointRateValue,
                              &bufferFrames,
                              &RtAudioStream::audioCallback,
                              this,
                              &options,
                              nullptr);
            audio->startStream();
            bufferFramesValue = bufferFrames;
            actualPointRateValue = audio->getStreamSampleRate();
            if (actualPointRateValue == 0) {
                actualPointRateValue = requestedPointRateValue;
            }

            running.store(true, std::memory_order_relaxed);
            return true;
        } catch (const RtAudioError&) {
            stop();
            return false;
        }
    }

    void stop() override {
        running.store(false, std::memory_order_relaxed);

        if (audio) {
            try {
                if (audio->isStreamRunning()) {
                    audio->abortStream();
                }
            } catch (const RtAudioError&) {
            }

            try {
                if (audio->isStreamOpen()) {
                    audio->closeStream();
                }
            } catch (const RtAudioError&) {
            }

            audio.reset();
        }

        if (countedAsioStream) {
            std::lock_guard lock(globalAsioStreamMutex());
            activeAsioStreamCount() = std::max(0, activeAsioStreamCount() - 1);
            countedAsioStream = false;
        }

        actualPointRateValue = 0;
        bufferFramesValue = rtAudioFramesPerBuffer;
    }

    std::uint32_t pointRate() const override {
        return actualPointRateValue;
    }

    std::uint32_t channelCount() const override {
        return channelCountValue;
    }

private:
    static std::mutex& globalAsioStreamMutex() {
        static std::mutex mutex;
        return mutex;
    }

    static int& activeAsioStreamCount() {
        static int count = 0;
        return count;
    }

    static int audioCallback(void* outputBuffer,
                             void* inputBuffer,
                             unsigned int frameCount,
                             double streamTime,
                             RtAudioStreamStatus status,
                             void* userData) {
        (void)inputBuffer;
        (void)streamTime;
        (void)status;

        auto* self = static_cast<RtAudioStream*>(userData);
        auto* output = static_cast<float*>(outputBuffer);
        if (self == nullptr || output == nullptr) {
            return 0;
        }

        const auto sampleCount = static_cast<std::size_t>(frameCount) * self->channelCountValue;
        std::fill_n(output, sampleCount, 0.0f);

        if (!self->callbackValue) {
            return 0;
        }

        const auto effectivePointRate = self->actualPointRateValue > 0
            ? self->actualPointRateValue
            : self->requestedPointRateValue;
        const auto leadTime = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(
                static_cast<double>(self->bufferFramesValue > 0 ? self->bufferFramesValue : frameCount) /
                static_cast<double>(effectivePointRate)));
        self->callbackValue(output, frameCount, self->channelCountValue, leadTime);
        return 0;
    }

    std::uint32_t deviceIdValue = 0;
    std::uint32_t requestedPointRateValue = 0;
    std::uint32_t actualPointRateValue = 0;
    std::uint32_t channelCountValue = 0;
    std::uint32_t bufferFramesValue = rtAudioFramesPerBuffer;
    AudioOutputCallback callbackValue;
    std::unique_ptr<RtAudio> audio;
    std::atomic<bool> running{false};
    bool countedAsioStream = false;
};

class RtAudioHost final : public AudioHost {
public:
    std::vector<AudioOutputDeviceInfo> listOutputDevices() override {
        std::vector<AudioOutputDeviceInfo> devices;
        if (!hasCompiledAsioApi()) {
            return devices;
        }

        try {
            RtAudio audio(RtAudio::WINDOWS_ASIO);
            audio.showWarnings(false);

            std::unordered_map<std::string, std::uint32_t> duplicateCountByName;
            const auto deviceCount = audio.getDeviceCount();
            devices.reserve(deviceCount);

            for (unsigned int deviceId = 0; deviceId < deviceCount; ++deviceId) {
                RtAudio::DeviceInfo info;
                try {
                    info = audio.getDeviceInfo(deviceId);
                } catch (const RtAudioError&) {
                    continue;
                }

                if (!info.probed || info.outputChannels == 0) {
                    continue;
                }

                const std::string baseName =
                    info.name.empty() ? "ASIO Device " + std::to_string(deviceId) : info.name;
                const auto duplicateIndex = ++duplicateCountByName[baseName];

                AudioOutputDeviceInfo device;
                device.backendDeviceId = deviceId;
                device.uid = makeRtAudioDeviceUid(baseName, duplicateIndex);
                device.label = makeRtAudioDeviceLabel(baseName, duplicateIndex);
                device.outputChannels = info.outputChannels;
                device.defaultPointRate = info.preferredSampleRate;
                device.supportedPointRates =
                    normalizeRtAudioPointRates(info.sampleRates, device.defaultPointRate);
                if (device.defaultPointRate == 0 && !device.supportedPointRates.empty()) {
                    device.defaultPointRate = device.supportedPointRates.front();
                }
                device.pointRateMutable = device.supportedPointRates.size() > 1;
                devices.push_back(std::move(device));
            }
        } catch (const RtAudioError&) {
            return {};
        }

        return devices;
    }

    std::unique_ptr<AudioOutputStream> openOutputStream(
        const AudioOutputDeviceInfo& device,
        std::uint32_t pointRateValue,
        AudioOutputCallback callback) override {
        if (device.outputChannels == 0 || pointRateValue == 0) {
            return nullptr;
        }

        return std::make_unique<RtAudioStream>(
            device.backendDeviceId,
            pointRateValue,
            device.outputChannels,
            std::move(callback));
    }
};

} // namespace
#endif

std::shared_ptr<AudioHost> createAudioHost() {
#if defined(__APPLE__)
    return std::make_shared<CoreAudioHost>();
#elif defined(_WIN32) && defined(LIBERA_HAS_RTAUDIO_AUDIO_HOST)
    return std::make_shared<RtAudioHost>();
#else
    class NullAudioHost final : public AudioHost {
    public:
        std::vector<AudioOutputDeviceInfo> listOutputDevices() override {
            return {};
        }

        std::unique_ptr<AudioOutputStream> openOutputStream(
            const AudioOutputDeviceInfo& device,
            std::uint32_t pointRateValue,
            AudioOutputCallback callback) override {
            (void)device;
            (void)pointRateValue;
            (void)callback;
            return nullptr;
        }
    };

    return std::make_shared<NullAudioHost>();
#endif
}

} // namespace libera::avb::detail

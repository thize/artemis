#include "micencoder.h"

#include <Limelight.h>

MicEncoder::MicEncoder()
    : m_AudioDevice(0),
      m_Encoder(nullptr),
      m_AudioSubsystemInitialized(false),
      m_SamplesBuffered(0),
      m_WindowStartTimestamp(0),
      m_WindowCapturedFrames(0),
      m_WindowSentFrames(0),
      m_WindowSentBytes(0),
      m_WindowSumSquares(0),
      m_WindowPeak(0),
      m_Capturing(false),
      m_SendingStopped(false),
      m_CapturedFps(0),
      m_SentFps(0),
      m_BitrateKbps(0),
      m_RmsLevel(0),
      m_PeakLevel(0),
      m_EncodeErrors(0),
      m_SendErrors(0)
{
    m_DeviceName[0] = 0;
}

MicEncoder::~MicEncoder()
{
    stop();
}

bool MicEncoder::start(const QString& deviceName)
{
    SDL_AudioSpec want, have;
    int error;

    SDL_assert(m_AudioDevice == 0);

    // SDL_InitSubSystem() is refcounted, so we hold our own reference for as
    // long as we're capturing rather than relying on the audio renderer's.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        return false;
    }

    m_AudioSubsystemInitialized = true;

    m_Encoder = opus_encoder_create(k_SampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (m_Encoder == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to create microphone encoder: %d",
                     error);
        stop();
        return false;
    }

    // Use CBR to keep each frame comfortably below the payload limit and to
    // avoid bitrate spikes on the control stream.
    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(k_Bitrate));
    opus_encoder_ctl(m_Encoder, OPUS_SET_VBR(0));

    SDL_zero(want);
    want.freq = k_SampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = k_SamplesPerFrame;
    want.callback = MicEncoder::captureCallback;
    want.userdata = this;

    // An empty device name means the user wants the system default device.
    QByteArray deviceNameUtf8 = deviceName.toUtf8();
    const char* device = deviceName.isEmpty() ? nullptr : deviceNameUtf8.constData();

    // Passing 0 for allowed_changes makes SDL convert the captured audio to
    // the format we asked for if the device can't provide it natively.
    m_AudioDevice = SDL_OpenAudioDevice(device, 1, &want, &have, 0);
    if (m_AudioDevice == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open microphone capture device: %s",
                     SDL_GetError());
        stop();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Capturing microphone audio from: %s",
                device != nullptr ? device : "default device");

    SDL_strlcpy(m_DeviceName,
                device != nullptr ? device : "Default device",
                sizeof(m_DeviceName));

    m_WindowStartTimestamp = SDL_GetTicks();
    m_Capturing.store(true, std::memory_order_relaxed);

    // Start capturing
    SDL_PauseAudioDevice(m_AudioDevice, 0);

    return true;
}

void MicEncoder::stop()
{
    // Stop reporting statistics before the last frames trickle out, so the
    // overlay doesn't display a microphone section for a dead device.
    m_Capturing.store(false, std::memory_order_relaxed);

    if (m_AudioDevice != 0) {
        // SDL_CloseAudioDevice() waits until our callback is no longer running,
        // so nothing below can race with a partially destroyed object.
        SDL_PauseAudioDevice(m_AudioDevice, 1);
        SDL_CloseAudioDevice(m_AudioDevice);
        m_AudioDevice = 0;
    }

    if (m_Encoder != nullptr) {
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
    }

    if (m_AudioSubsystemInitialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_AudioSubsystemInitialized = false;
    }

    m_SamplesBuffered = 0;
    m_WindowStartTimestamp = 0;
    m_WindowCapturedFrames = 0;
    m_WindowSentFrames = 0;
    m_WindowSentBytes = 0;
    m_WindowSumSquares = 0;
    m_WindowPeak = 0;

    m_SendingStopped.store(false, std::memory_order_relaxed);
    m_CapturedFps.store(0, std::memory_order_relaxed);
    m_SentFps.store(0, std::memory_order_relaxed);
    m_BitrateKbps.store(0, std::memory_order_relaxed);
    m_RmsLevel.store(0, std::memory_order_relaxed);
    m_PeakLevel.store(0, std::memory_order_relaxed);
    m_EncodeErrors.store(0, std::memory_order_relaxed);
    m_SendErrors.store(0, std::memory_order_relaxed);
}

void MicEncoder::getStats(MIC_STATS& stats) const
{
    stats.active = m_Capturing.load(std::memory_order_relaxed);
    stats.sendingStopped = m_SendingStopped.load(std::memory_order_relaxed);
    stats.capturedFps = m_CapturedFps.load(std::memory_order_relaxed);
    stats.sentFps = m_SentFps.load(std::memory_order_relaxed);
    stats.bitrateKbps = m_BitrateKbps.load(std::memory_order_relaxed);
    stats.rmsLevel = m_RmsLevel.load(std::memory_order_relaxed);
    stats.peakLevel = m_PeakLevel.load(std::memory_order_relaxed);
    stats.encodeErrors = m_EncodeErrors.load(std::memory_order_relaxed);
    stats.sendErrors = m_SendErrors.load(std::memory_order_relaxed);

    SDL_strlcpy(stats.deviceName, m_DeviceName, sizeof(stats.deviceName));
}

void MicEncoder::captureCallback(void* userdata, Uint8* stream, int len)
{
    MicEncoder* me = reinterpret_cast<MicEncoder*>(userdata);
    const float* samples = reinterpret_cast<const float*>(stream);
    int sampleCount = len / sizeof(float);

    // The device stays open after we stop sending, since we can't close it
    // from inside its own callback.
    if (me->m_SendingStopped.load(std::memory_order_relaxed)) {
        return;
    }

    // SDL will normally hand us exactly one frame worth of samples, but we
    // accumulate anyway to tolerate a different buffer size from the device.
    while (sampleCount > 0) {
        int samplesToCopy = SDL_min(sampleCount, k_SamplesPerFrame - me->m_SamplesBuffered);

        SDL_memcpy(&me->m_FrameBuffer[me->m_SamplesBuffered],
                   samples,
                   samplesToCopy * sizeof(float));

        me->m_SamplesBuffered += samplesToCopy;
        samples += samplesToCopy;
        sampleCount -= samplesToCopy;

        if (me->m_SamplesBuffered == k_SamplesPerFrame) {
            me->m_SamplesBuffered = 0;
            me->encodeAndSendFrame();

            if (me->m_SendingStopped.load(std::memory_order_relaxed)) {
                return;
            }
        }
    }
}

void MicEncoder::encodeAndSendFrame()
{
    unsigned char packet[k_MaxPacketSize];
    float sumSquares = 0;
    float peak = 0;

    // Measure the input level of this frame while it's still hot in the cache
    for (int i = 0; i < k_SamplesPerFrame; i++) {
        float sample = m_FrameBuffer[i];
        float magnitude = SDL_fabsf(sample);

        sumSquares += sample * sample;
        peak = SDL_max(peak, magnitude);
    }

    m_WindowCapturedFrames++;
    m_WindowSumSquares += sumSquares;
    m_WindowPeak = SDL_max(m_WindowPeak, peak);

    int packetSize = opus_encode_float(m_Encoder,
                                       m_FrameBuffer,
                                       k_SamplesPerFrame,
                                       packet,
                                       sizeof(packet));
    if (packetSize < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to encode microphone audio: %d",
                     packetSize);
        m_EncodeErrors.fetch_add(1, std::memory_order_relaxed);
        stopSending();
        return;
    }

    int err = LiSendMicrophoneData(packet, packetSize);
    if (err == LI_ERR_UNSUPPORTED) {
        // The host didn't accept our microphone offer, so there's no point in
        // encoding any more audio. Log this once rather than every 10 ms.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host does not support microphone passthrough");
        stopSending();
        return;
    }
    else if (err != 0) {
        // Any other failure is a transient send error on an unreliable channel,
        // so we drop the frame silently instead of logging every 10 ms.
        m_SendErrors.fetch_add(1, std::memory_order_relaxed);
    }
    else {
        m_WindowSentFrames++;
        m_WindowSentBytes += packetSize;
    }

    publishStats();
}

void MicEncoder::publishStats()
{
    Uint32 now = SDL_GetTicks();

    if (!SDL_TICKS_PASSED(now, m_WindowStartTimestamp + k_StatsWindowMs)) {
        return;
    }

    float windowSeconds = (float)(now - m_WindowStartTimestamp) / 1000;

    m_CapturedFps.store(m_WindowCapturedFrames / windowSeconds, std::memory_order_relaxed);
    m_SentFps.store(m_WindowSentFrames / windowSeconds, std::memory_order_relaxed);
    m_BitrateKbps.store((float)m_WindowSentBytes * 8 / windowSeconds / 1000, std::memory_order_relaxed);
    m_RmsLevel.store(SDL_sqrtf(m_WindowSumSquares / (m_WindowCapturedFrames * k_SamplesPerFrame)), std::memory_order_relaxed);
    m_PeakLevel.store(m_WindowPeak, std::memory_order_relaxed);

    m_WindowStartTimestamp = now;
    m_WindowCapturedFrames = 0;
    m_WindowSentFrames = 0;
    m_WindowSentBytes = 0;
    m_WindowSumSquares = 0;
    m_WindowPeak = 0;
}

void MicEncoder::stopSending()
{
    m_SendingStopped.store(true, std::memory_order_relaxed);

    // The measurements below are stale as soon as we stop feeding the encoder,
    // so zero them rather than leaving the last window on screen forever.
    m_CapturedFps.store(0, std::memory_order_relaxed);
    m_SentFps.store(0, std::memory_order_relaxed);
    m_BitrateKbps.store(0, std::memory_order_relaxed);
    m_RmsLevel.store(0, std::memory_order_relaxed);
    m_PeakLevel.store(0, std::memory_order_relaxed);
}

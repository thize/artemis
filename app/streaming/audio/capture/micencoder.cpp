#include "micencoder.h"

#include <Limelight.h>

MicEncoder::MicEncoder()
    : m_AudioDevice(0),
      m_Encoder(nullptr),
      m_AudioSubsystemInitialized(false),
      m_SamplesBuffered(0),
      m_SendingStopped(false)
{
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

    // Start capturing
    SDL_PauseAudioDevice(m_AudioDevice, 0);

    return true;
}

void MicEncoder::stop()
{
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
    m_SendingStopped = false;
}

void MicEncoder::captureCallback(void* userdata, Uint8* stream, int len)
{
    MicEncoder* me = reinterpret_cast<MicEncoder*>(userdata);
    const float* samples = reinterpret_cast<const float*>(stream);
    int sampleCount = len / sizeof(float);

    // The device stays open after we stop sending, since we can't close it
    // from inside its own callback.
    if (me->m_SendingStopped) {
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

            if (me->m_SendingStopped) {
                return;
            }
        }
    }
}

void MicEncoder::encodeAndSendFrame()
{
    unsigned char packet[k_MaxPacketSize];

    int packetSize = opus_encode_float(m_Encoder,
                                       m_FrameBuffer,
                                       k_SamplesPerFrame,
                                       packet,
                                       sizeof(packet));
    if (packetSize < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to encode microphone audio: %d",
                     packetSize);
        m_SendingStopped = true;
        return;
    }

    int err = LiSendMicrophoneData(packet, packetSize);
    if (err == LI_ERR_UNSUPPORTED) {
        // The host didn't accept our microphone offer, so there's no point in
        // encoding any more audio. Log this once rather than every 10 ms.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Host does not support microphone passthrough");
        m_SendingStopped = true;
    }

    // Any other failure is a transient send error on an unreliable channel,
    // so we drop the frame silently instead of logging every 10 ms.
}

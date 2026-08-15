#include "microphonedevices.h"

#include <cmath>

MicrophoneDevices::MicrophoneDevices()
    : level(0.0),
      audioDevice(0),
      audioSubsystemInitialized(false),
      smoothedLevel(0.0f),
      envelope(0.0f)
{
    levelTimer.setInterval(k_LevelUpdateIntervalMs);
    connect(&levelTimer, &QTimer::timeout, this, &MicrophoneDevices::publishLevel);

    refresh();
}

MicrophoneDevices::~MicrophoneDevices()
{
    stopMonitoring();
}

void MicrophoneDevices::refresh()
{
    captureDevices.clear();

    // The audio subsystem is only initialized for the duration of the
    // enumeration, since the audio renderer expects to own it while streaming.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        emit captureDevicesChanged();
        return;
    }

    int deviceCount = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < deviceCount; i++) {
        const char* deviceName = SDL_GetAudioDeviceName(i, 1);
        if (deviceName != nullptr) {
            captureDevices.append(QString::fromUtf8(deviceName));
        }
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    emit captureDevicesChanged();
}

void MicrophoneDevices::startMonitoring(const QString& deviceName)
{
    SDL_AudioSpec want, have;

    // Close whatever we had open before, so callers can switch devices by
    // simply starting again.
    stopMonitoring();

    // SDL_InitSubSystem() is refcounted, so we hold our own reference for as
    // long as the device is open rather than relying on anyone else's.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        return;
    }

    audioSubsystemInitialized = true;

    SDL_zero(want);
    want.freq = k_SampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = k_SamplesPerFrame;
    want.callback = MicrophoneDevices::monitorCallback;
    want.userdata = this;

    // An empty device name means the user wants the system default device.
    QByteArray deviceNameUtf8 = deviceName.toUtf8();
    const char* device = deviceName.isEmpty() ? nullptr : deviceNameUtf8.constData();

    // Passing 0 for allowed_changes makes SDL convert the captured audio to
    // the format we asked for if the device can't provide it natively.
    audioDevice = SDL_OpenAudioDevice(device, 1, &want, &have, 0);
    if (audioDevice == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to open microphone capture device: %s",
                     SDL_GetError());
        stopMonitoring();
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Monitoring microphone level from: %s",
                device != nullptr ? device : "default device");

    // Start capturing
    SDL_PauseAudioDevice(audioDevice, 0);

    levelTimer.start();
}

void MicrophoneDevices::stopMonitoring()
{
    levelTimer.stop();

    if (audioDevice != 0) {
        // SDL_CloseAudioDevice() waits until our callback is no longer running,
        // so the envelope state below can't be touched by the capture thread
        // anymore once this returns.
        SDL_PauseAudioDevice(audioDevice, 1);
        SDL_CloseAudioDevice(audioDevice);
        audioDevice = 0;
    }

    if (audioSubsystemInitialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audioSubsystemInitialized = false;
    }

    envelope = 0.0f;
    smoothedLevel.store(0.0f, std::memory_order_relaxed);

    if (level != 0.0) {
        level = 0.0;
        emit levelChanged();
    }
}

void MicrophoneDevices::monitorCallback(void* userdata, Uint8* stream, int len)
{
    MicrophoneDevices* me = reinterpret_cast<MicrophoneDevices*>(userdata);
    const float* samples = reinterpret_cast<const float*>(stream);
    int sampleCount = len / sizeof(float);

    if (sampleCount <= 0) {
        return;
    }

    double sumOfSquares = 0.0;
    for (int i = 0; i < sampleCount; i++) {
        sumOfSquares += (double)samples[i] * samples[i];
    }

    // Human loudness perception is logarithmic, so the RMS of the block is
    // converted to dBFS and mapped onto 0.0 - 1.0 between the floor and full
    // scale. A linear RMS bar would barely leave the left edge at normal
    // speaking volume.
    float rms = (float)std::sqrt(sumOfSquares / sampleCount);
    float target = 0.0f;
    if (rms > 0.0f) {
        float db = 20.0f * std::log10(rms);
        target = (db - k_FloorDb) / -k_FloorDb;
        target = SDL_max(0.0f, SDL_min(1.0f, target));
    }

    // Exponential envelope follower. The coefficient is derived from the
    // duration of this block, so the attack and release stay consistent even
    // if the device hands us a different buffer size than we asked for.
    float blockSeconds = (float)sampleCount / k_SampleRate;
    float timeConstant = target > me->envelope ? k_AttackSeconds : k_ReleaseSeconds;
    float coefficient = 1.0f - std::exp(-blockSeconds / timeConstant);

    me->envelope += (target - me->envelope) * coefficient;

    // The release is asymptotic, so snap to zero once the remainder is well
    // below one pixel of the meter.
    if (me->envelope < 0.001f) {
        me->envelope = 0.0f;
    }

    // Qt signals can't be emitted from the SDL capture thread, so the GUI
    // thread picks this up from its own timer instead.
    me->smoothedLevel.store(me->envelope, std::memory_order_relaxed);
}

void MicrophoneDevices::publishLevel()
{
    qreal newLevel = smoothedLevel.load(std::memory_order_relaxed);

    if (newLevel != level) {
        level = newLevel;
        emit levelChanged();
    }
}

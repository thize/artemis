#pragma once

#include "SDL_compat.h"

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <atomic>

class MicrophoneDevices : public QObject
{
    Q_OBJECT

public:
    MicrophoneDevices();

    ~MicrophoneDevices();

    Q_PROPERTY(QStringList captureDevices MEMBER captureDevices NOTIFY captureDevicesChanged)

    // Normalized microphone level between 0.0 and 1.0. It stays at 0.0 while
    // no monitoring is in progress.
    Q_PROPERTY(qreal level MEMBER level NOTIFY levelChanged)

    Q_INVOKABLE void refresh();

    // Opens the given capture device (empty means the system default) and
    // starts updating the level property. Any device opened by a previous
    // call is closed first, so this can also be used to follow device changes.
    Q_INVOKABLE void startMonitoring(const QString& deviceName);

    Q_INVOKABLE void stopMonitoring();

signals:
    void captureDevicesChanged();
    void levelChanged();

private:
    static void monitorCallback(void* userdata, Uint8* stream, int len);

    void publishLevel();

    // Capturing at the same rate and format as the streaming microphone
    // encoder keeps the meter representative of what the host will hear.
    static constexpr int k_SampleRate = 48000;
    static constexpr int k_SamplesPerFrame = 480;

    // A linear RMS meter sits near zero for normal speech, so the level is
    // mapped from dBFS onto 0.0 - 1.0 with this floor, like the level bars
    // used by voice chat applications.
    static constexpr float k_FloorDb = -60.0f;

    // Time constants of the envelope follower. The fast attack makes the bar
    // react immediately when the user starts talking, while the slow release
    // keeps it from flickering between syllables.
    static constexpr float k_AttackSeconds = 0.03f;
    static constexpr float k_ReleaseSeconds = 0.30f;

    // The level property is refreshed at roughly 30 Hz, which is smooth enough
    // for the meter without waking the GUI thread on every capture callback.
    static constexpr int k_LevelUpdateIntervalMs = 33;

    QStringList captureDevices;
    qreal level;

    SDL_AudioDeviceID audioDevice;
    bool audioSubsystemInitialized;
    QTimer levelTimer;

    // Written by the SDL capture thread and read by the GUI thread. The
    // envelope itself is only touched by the capture thread, and
    // SDL_CloseAudioDevice() waits for that thread before we reset it.
    std::atomic<float> smoothedLevel;
    float envelope;
};

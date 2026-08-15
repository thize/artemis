#pragma once

#include "SDL_compat.h"

#include <QString>

#include <atomic>

#include <opus.h>

typedef struct _MIC_STATS {
    // Set while a capture device is open, whether or not we're still sending
    bool active;
    bool sendingStopped;

    // Measured over the last completed measurement window
    float capturedFps;
    float sentFps;
    float bitrateKbps;
    float rmsLevel;
    float peakLevel;

    // Accumulated over the whole session
    uint32_t encodeErrors;
    uint32_t sendErrors;

    char deviceName[128];
} MIC_STATS, *PMIC_STATS;

// Captures audio from a microphone and streams it to the host as Opus frames.
//
// The host must have accepted the microphone offer (LI_FF_MICROPHONE) before
// capture is started, since LiSendMicrophoneData() is only usable in that case.
// Only one instance may be capturing at a time, because LiSendMicrophoneData()
// cannot be called concurrently from multiple threads.
class MicEncoder
{
public:
    MicEncoder();

    ~MicEncoder();

    bool start(const QString& deviceName);

    void stop();

    // May be called from any thread while capture is running.
    void getStats(MIC_STATS& stats) const;

private:
    static void captureCallback(void* userdata, Uint8* stream, int len);

    void encodeAndSendFrame();

    void publishStats();

    void stopSending();

    // The host expects mono 48 KHz Opus frames covering 10 ms of audio each.
    // At 32 Kbps, those stay well below the 200 byte payload limit imposed by
    // LiSendMicrophoneData().
    static constexpr int k_SampleRate = 48000;
    static constexpr int k_SamplesPerFrame = 480;
    static constexpr int k_Bitrate = 32000;
    static constexpr int k_MaxPacketSize = 200;

    // Rates and levels are averaged over a window of this length, matching the
    // cadence at which the performance overlay is refreshed.
    static constexpr int k_StatsWindowMs = 1000;

    SDL_AudioDeviceID m_AudioDevice;
    OpusEncoder* m_Encoder;
    bool m_AudioSubsystemInitialized;

    // These are only touched by the SDL capture thread. SDL_CloseAudioDevice()
    // waits for that thread to finish, so stop() can safely tear down the
    // encoder once the device is closed.
    float m_FrameBuffer[k_SamplesPerFrame];
    int m_SamplesBuffered;
    Uint32 m_WindowStartTimestamp;
    uint32_t m_WindowCapturedFrames;
    uint32_t m_WindowSentFrames;
    uint32_t m_WindowSentBytes;
    float m_WindowSumSquares;
    float m_WindowPeak;

    // These are written by the SDL capture thread and read by the thread that
    // renders the performance overlay. They are lock-free by design, because
    // the capture callback must never block on the overlay.
    std::atomic<bool> m_Capturing;
    std::atomic<bool> m_SendingStopped;
    std::atomic<float> m_CapturedFps;
    std::atomic<float> m_SentFps;
    std::atomic<float> m_BitrateKbps;
    std::atomic<float> m_RmsLevel;
    std::atomic<float> m_PeakLevel;
    std::atomic<uint32_t> m_EncodeErrors;
    std::atomic<uint32_t> m_SendErrors;

    // Written by start() before the capture device is opened and never
    // modified afterwards, so readers always observe a valid string.
    char m_DeviceName[sizeof(MIC_STATS::deviceName)];
};

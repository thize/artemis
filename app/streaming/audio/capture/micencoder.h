#pragma once

#include "SDL_compat.h"

#include <QString>

#include <opus.h>

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

private:
    static void captureCallback(void* userdata, Uint8* stream, int len);

    void encodeAndSendFrame();

    // The host expects mono 48 KHz Opus frames covering 10 ms of audio each.
    // At 32 Kbps, those stay well below the 200 byte payload limit imposed by
    // LiSendMicrophoneData().
    static constexpr int k_SampleRate = 48000;
    static constexpr int k_SamplesPerFrame = 480;
    static constexpr int k_Bitrate = 32000;
    static constexpr int k_MaxPacketSize = 200;

    SDL_AudioDeviceID m_AudioDevice;
    OpusEncoder* m_Encoder;
    bool m_AudioSubsystemInitialized;

    // These are only touched by the SDL capture thread. SDL_CloseAudioDevice()
    // waits for that thread to finish, so stop() can safely tear down the
    // encoder once the device is closed.
    float m_FrameBuffer[k_SamplesPerFrame];
    int m_SamplesBuffered;
    bool m_SendingStopped;
};

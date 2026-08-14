#include "microphonedevices.h"

#include "SDL_compat.h"

MicrophoneDevices::MicrophoneDevices()
{
    refresh();
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

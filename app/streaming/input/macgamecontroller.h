#pragma once

// Helpers for Apple's GameController framework on macOS.
//
// SDL's IOKit joystick driver skips every HID gamepad that the GameController
// framework claims to support, expecting SDL's MFi driver to receive it from the
// framework instead. Some gamepads (seen with the 8BitDo Ultimate 2C over
// Bluetooth on macOS 27) are claimed but never delivered by the framework, which
// leaves SDL without any gamepad at all.
namespace MacGameController
{
    // Starts the framework's controller discovery. The framework fills its
    // controller list asynchronously after first use, so call this early.
    void startDiscovery();

    // Sets SDL_HINT_JOYSTICK_MFI for the next initialization of SDL's joystick
    // subsystem. The MFi driver is disabled when the framework claims more HID
    // gamepads than it delivers, so SDL's IOKit driver handles them instead.
    void configureSdlJoystickDrivers();
}

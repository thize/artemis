#include "macgamecontroller.h"

#import <GameController/GameController.h>

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>

#include <SDL.h>

#include <vector>

namespace
{
    CFDictionaryRef createUsageMatch(int usagePage, int usage)
    {
        CFNumberRef page = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usagePage);
        CFNumberRef use = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
        const void* keys[] = { CFSTR(kIOHIDDeviceUsagePageKey), CFSTR(kIOHIDDeviceUsageKey) };
        const void* values[] = { page, use };
        CFDictionaryRef match = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CFRelease(page);
        CFRelease(use);
        return match;
    }

    // Counts the HID gamepads that the GameController framework claims to
    // support. These are the devices SDL's IOKit joystick driver will skip.
    int countClaimedHidGamepads()
    {
        if (@available(macOS 11.0, *)) {
            IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
            if (manager == NULL) {
                return 0;
            }

            CFDictionaryRef matches[3] = {
                createUsageMatch(kHIDPage_GenericDesktop, kHIDUsage_GD_Joystick),
                createUsageMatch(kHIDPage_GenericDesktop, kHIDUsage_GD_GamePad),
                createUsageMatch(kHIDPage_GenericDesktop, kHIDUsage_GD_MultiAxisController)
            };
            CFArrayRef matchArray = CFArrayCreate(kCFAllocatorDefault, (const void**)matches, 3,
                                                  &kCFTypeArrayCallBacks);
            IOHIDManagerSetDeviceMatchingMultiple(manager, matchArray);
            CFRelease(matchArray);
            for (int i = 0; i < 3; i++) {
                CFRelease(matches[i]);
            }

            int claimed = 0;
            CFSetRef devices = IOHIDManagerCopyDevices(manager);
            if (devices != NULL) {
                CFIndex count = CFSetGetCount(devices);
                if (count > 0) {
                    std::vector<const void*> deviceList(count);
                    CFSetGetValues(devices, deviceList.data());
                    for (CFIndex i = 0; i < count; i++) {
                        if ([GCController supportsHIDDevice:(IOHIDDeviceRef)deviceList[i]]) {
                            claimed++;
                        }
                    }
                }
                CFRelease(devices);
            }

            CFRelease(manager);
            return claimed;
        }

        return 0;
    }
}

void MacGameController::startDiscovery()
{
    // Merely asking for the list makes the framework start looking for controllers
    @autoreleasepool {
        (void)[GCController controllers];
    }
}

void MacGameController::configureSdlJoystickDrivers()
{
    // An explicit environment variable set by the user always wins
    if (SDL_getenv("SDL_JOYSTICK_MFI") != nullptr) {
        return;
    }

    @autoreleasepool {
        int claimed = countClaimedHidGamepads();
        NSUInteger delivered = [GCController controllers].count;

        if (claimed > (int)delivered) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GameController framework claims %d HID gamepad(s) but delivers only %lu; disabling SDL's MFi joystick driver",
                        claimed, (unsigned long)delivered);
            SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
        }
        else {
            // The hint is process-wide, so always set it explicitly in case an
            // earlier call has set it to "0"
            SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "1");
        }
    }
}

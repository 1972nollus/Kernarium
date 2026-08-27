// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_battery.hpp"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <algorithm>
#include <cmath>
#include <memory>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#include <dispatch/dispatch.h>
#else
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/ps/IOPSKeys.h>
#endif

namespace uml::platform {
namespace {

HostBatteryState fallbackState() noexcept
{
    return {};
}

#if TARGET_OS_IPHONE

HostBatteryState readIOSBattery() noexcept
{
    UIDevice* device = UIDevice.currentDevice;
    device.batteryMonitoringEnabled = YES;
    const float level = device.batteryLevel;
    const UIDeviceBatteryState batteryState = device.batteryState;
    if (!std::isfinite(level) || level < 0.0F ||
        batteryState == UIDeviceBatteryStateUnknown) {
        return fallbackState();
    }

    HostBatteryState result;
    result.percentage = static_cast<std::uint8_t>(std::clamp(
        std::lround(static_cast<double>(level) * 100.0), 0L, 100L));
    switch (batteryState) {
    case UIDeviceBatteryStateCharging:
        result.status = result.percentage == 100
            ? HostBatteryStatus::full
            : HostBatteryStatus::charging;
        result.externalPower = true;
        break;
    case UIDeviceBatteryStateFull:
        result.status = HostBatteryStatus::full;
        result.externalPower = true;
        break;
    case UIDeviceBatteryStateUnplugged:
        result.status = HostBatteryStatus::discharging;
        result.externalPower = false;
        break;
    case UIDeviceBatteryStateUnknown:
        return fallbackState();
    }
    return result;
}

class AppleHostBatteryProvider final : public HostBatteryProvider {
public:
    HostBatteryState state() noexcept override
    {
        if (NSThread.isMainThread)
            return readIOSBattery();
        __block HostBatteryState result = fallbackState();
        dispatch_sync(dispatch_get_main_queue(), ^{
          result = readIOSBattery();
        });
        return result;
    }
};

#else

bool dictionaryBoolean(CFDictionaryRef dictionary, CFStringRef key) noexcept
{
    const auto value = static_cast<CFTypeRef>(
        CFDictionaryGetValue(dictionary, key));
    return value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID() &&
        CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

bool dictionaryInteger(
    CFDictionaryRef dictionary,
    CFStringRef key,
    int& result) noexcept
{
    const auto value = static_cast<CFTypeRef>(
        CFDictionaryGetValue(dictionary, key));
    return value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID() &&
        CFNumberGetValue(
            static_cast<CFNumberRef>(value), kCFNumberIntType, &result);
}

class AppleHostBatteryProvider final : public HostBatteryProvider {
public:
    HostBatteryState state() noexcept override
    {
        const auto information = IOPSCopyPowerSourcesInfo();
        if (information == nullptr)
            return fallbackState();
        const auto sources = IOPSCopyPowerSourcesList(information);
        if (sources == nullptr) {
            CFRelease(information);
            return fallbackState();
        }

        HostBatteryState result = fallbackState();
        bool found = false;
        const CFIndex count = CFArrayGetCount(sources);
        for (CFIndex index = 0; index < count && !found; ++index) {
            const auto source = CFArrayGetValueAtIndex(sources, index);
            const auto description = IOPSGetPowerSourceDescription(
                information, source);
            if (description == nullptr)
                continue;
            const auto transport = static_cast<CFStringRef>(
                CFDictionaryGetValue(description, CFSTR(kIOPSTransportTypeKey)));
            if (transport == nullptr ||
                CFStringCompare(
                    transport, CFSTR(kIOPSInternalType), 0) !=
                    kCFCompareEqualTo) {
                continue;
            }

            int current {};
            int maximum {};
            if (!dictionaryInteger(
                    description, CFSTR(kIOPSCurrentCapacityKey), current) ||
                !dictionaryInteger(
                    description, CFSTR(kIOPSMaxCapacityKey), maximum) ||
                maximum <= 0) {
                continue;
            }
            result.percentage = static_cast<std::uint8_t>(std::clamp(
                (current * 100 + maximum / 2) / maximum, 0, 100));
            const auto powerState = static_cast<CFStringRef>(
                CFDictionaryGetValue(description, CFSTR(kIOPSPowerSourceStateKey)));
            result.externalPower = powerState != nullptr &&
                CFStringCompare(powerState, CFSTR(kIOPSACPowerValue), 0) ==
                    kCFCompareEqualTo;
            const bool charging = dictionaryBoolean(
                description, CFSTR(kIOPSIsChargingKey));
            result.status = charging
                ? HostBatteryStatus::charging
                : result.externalPower && result.percentage == 100
                ? HostBatteryStatus::full
                : result.externalPower
                ? HostBatteryStatus::notCharging
                : HostBatteryStatus::discharging;
            found = true;
        }
        CFRelease(sources);
        CFRelease(information);
        return found ? result : fallbackState();
    }
};

#endif

} // namespace

std::unique_ptr<HostBatteryProvider> makeAppleHostBatteryProvider()
{
    return std::make_unique<AppleHostBatteryProvider>();
}

} // namespace uml::platform

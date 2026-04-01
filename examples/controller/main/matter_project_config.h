#pragma once

#include <sdkconfig.h>

#ifdef CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
// CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE is defined by
// CHIPDevicePlatformConfig.h as CONFIG_ENABLE_ESP32_BLE_CONTROLLER.
// Enable that Kconfig option (in sdkconfig.defaults) to activate combined mode.

// Enable or disable whether this device advertises as a commissioner.
// #define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY 1
#endif // CONFIG_ESP_MATTER_COMMISSIONER_ENABLE

// Number of devices a controller can be simultaneously connected to
#define CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES 8

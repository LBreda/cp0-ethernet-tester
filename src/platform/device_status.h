/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/// @file device_status.h
/// @brief Snapshot of device connectivity and power state (Wi-Fi, Ethernet, battery).

#pragma once

#include <cstdint>

namespace platform {

/// Point-in-time hardware status; percentages are -1 when unknown.
struct DeviceStatus {
    bool wifi_present{false};
    bool wifi_connected{false};
    int32_t wifi_strength_percent{-1};
    bool ethernet_present{false};
    bool ethernet_connected{false};
    bool battery_present{false};
    bool battery_charging{false};
    int32_t battery_percent{-1};
};

/// Reads the current status from the system (sysfs/network interfaces).
DeviceStatus read_device_status();

} // namespace platform

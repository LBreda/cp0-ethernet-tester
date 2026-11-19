/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/// @file screenshot_service.h
/// @brief Captures the active LVGL screen to an image file.

#pragma once

#include <string>

namespace platform::screenshot {

/// Outcome of a capture: output path on success, error message otherwise.
struct CaptureResult {
    bool success{false};
    std::string path;
    std::string error;
};

/// Saves a screenshot of the active screen.
CaptureResult capture_active_screen();

} // namespace platform::screenshot

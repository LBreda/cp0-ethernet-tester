/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file application_config.h
 * @brief Persistent user-facing application settings and their load/save helpers.
 */

#pragma once

#include <string>

namespace app {

/** @brief User preferences persisted between application runs. */
struct ApplicationConfig {
    bool dark_mode = false;
};

/**
 * @brief Loads the configuration from @p path into @p config.
 * @param error Set to a human-readable message on failure.
 * @return true on success.
 */
bool load_application_config(const std::string& path,
                             ApplicationConfig& config,
                             std::string& error);
/**
 * @brief Writes @p config to @p path.
 * @param error Set to a human-readable message on failure.
 * @return true on success.
 */
bool save_application_config(const std::string& path,
                             const ApplicationConfig& config,
                             std::string& error);

} // namespace app

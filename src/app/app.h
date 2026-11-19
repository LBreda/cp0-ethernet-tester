/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app.h
 * @brief Application entry object that owns initialization and the main loop.
 */

#pragma once

namespace app {

/** @brief Top-level application: sets up LVGL, screens and view model, then runs. */
class Application {
public:
    /**
     * @brief Runs the application until quit is requested.
     * @return Process exit code.
     */
    int run();
};

} // namespace app

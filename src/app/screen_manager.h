/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file screen_manager.h
 * @brief Creates and switches the active LVGL screen according to the view model's page.
 */

#pragma once

#include "asset_manager.h"
#include "base_screen.h"
#include "base_viewmodel.h"
#include "lvgl.h"

#include <memory>

namespace app {

/**
 * @brief Owns the current screen and swaps it when the requested page changes.
 *
 * Page switches are deferred so a screen is never destroyed from inside its own event callback.
 */
class ScreenManager {
public:
    ScreenManager(viewmodel::BaseViewModel& view_model, AssetManager& assets);
    ~ScreenManager();

    ScreenManager(const ScreenManager&) = delete;
    ScreenManager& operator=(const ScreenManager&) = delete;

    /** @brief Observes the view model's page subject and loads the initial screen. */
    void start();
    /** @brief Immediately loads the Apple screen. */
    void show_apple_screen();
    /** @brief Immediately loads the Butter screen. */
    void show_butter_screen();
    /** @brief Schedules a switch to @p page on the next LVGL cycle. */
    void request_page(model::AppPage page);
    /** @brief Loads the pending requested page if it differs from the loaded one. */
    void flush_requested_page();
    /** @brief Root LVGL object of the current screen, or nullptr. */
    lv_obj_t* current_screen() const;

private:
    void load_screen(std::unique_ptr<screen::BaseScreen> next_screen);

    viewmodel::BaseViewModel& view_model_;
    AssetManager& assets_;
    std::unique_ptr<screen::BaseScreen> current_screen_;
    lv_observer_t* page_observer_ = nullptr;
    model::AppPage requested_page_ = model::AppPage::Apple;
    model::AppPage loaded_page_ = model::AppPage::Apple;
    bool has_loaded_page_ = false;
    bool page_switch_scheduled_ = false;
};

} // namespace app

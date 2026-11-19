/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_viewmodel.h"
#include "titlebar.h"
#include "lvgl.h"

#include <memory>

namespace app {
class AssetManager;
}

namespace screen {

/**
 * @brief Base class for full-screen pages.
 *
 * Creates a root object with a TitleBar and a content area; subclasses
 * populate the content area in build_content().
 */
class BaseScreen {
public:
    BaseScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    virtual ~BaseScreen();

    BaseScreen(const BaseScreen&) = delete;
    BaseScreen& operator=(const BaseScreen&) = delete;

    /// Builds the screen layout and calls build_content().
    void init();
    /// Returns the screen's root LVGL object.
    lv_obj_t* root() const;

protected:
    /// Populates the content area below the title bar.
    virtual void build_content(lv_obj_t* content) = 0;

    viewmodel::BaseViewModel& view_model();
    app::AssetManager& assets();

private:
    viewmodel::BaseViewModel& view_model_;
    app::AssetManager& assets_;
    lv_obj_t* root_{nullptr};
    lv_obj_t* content_{nullptr};
    std::unique_ptr<view::widgets::TitleBar> title_bar_;
};

} // namespace screen

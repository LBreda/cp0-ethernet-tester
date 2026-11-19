/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_model.h"
#include "base_widget.h"

#include <array>

namespace app {
class AssetManager;
}

namespace viewmodel {
class BaseViewModel;
}

namespace view::widgets {

/**
 * @brief Modal overlay listing the keyboard shortcuts for a page.
 *
 * Clicking the backdrop dismisses the popup. Colors follow the theme
 * exposed by the view model.
 */
class HelpPopup : public BaseWidgets {
public:
    HelpPopup(lv_obj_t* parent,
              viewmodel::BaseViewModel& view_model,
              app::AssetManager& assets);

    /// Creates the (initially hidden) backdrop and panel.
    void build() override;
    /// Fills the shortcut rows for @p page and shows the popup.
    void show(model::AppPage page);
    /// Hides the popup.
    void hide();
    /// Returns true if the popup is currently shown.
    bool visible() const;

private:
    struct RowWidgets {
        lv_obj_t* row{nullptr};
        lv_obj_t* key{nullptr};
        lv_obj_t* action{nullptr};
    };

    void update_rows(model::AppPage page);
    void apply_theme();
    static void backdrop_clicked_cb(lv_event_t* event);
    static void theme_observer_cb(lv_observer_t* observer, lv_subject_t* subject);

    viewmodel::BaseViewModel& view_model_;
    app::AssetManager& assets_;
    lv_obj_t* panel_{nullptr};
    lv_obj_t* title_{nullptr};
    lv_obj_t* page_badge_{nullptr};
    lv_obj_t* footer_{nullptr};
    std::array<RowWidgets, 6> rows_{};
};

} // namespace view::widgets

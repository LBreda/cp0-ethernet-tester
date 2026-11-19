/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */


#pragma once

#include "base_widget.h"
#include "base_viewmodel.h"

#include <cstdint>
#include <string>

namespace view::widgets {

/**
 * @brief Themed button displaying an icon glyph, with an optional
 *        indicator and enabled/disabled state.
 */
class IconButton : public BaseWidgets {

public:
    /**
     * @param width, height   Button size in pixels.
     * @param text            Icon glyph or label text.
     * @param font            Font used to render @p text.
     * @param light_color     Accent color in light mode.
     * @param dark_color      Accent color in dark mode.
     * @param click_cb        Optional LVGL click callback.
     * @param user_data       User data passed to @p click_cb.
     * @param show_indicator  Whether to draw the indicator element.
     */
    IconButton(lv_obj_t* parent,
               viewmodel::BaseViewModel& view_model,
               int32_t width,
               int32_t height,
               const char* text,
               const lv_font_t* font,
               lv_color_t light_color,
               lv_color_t dark_color,
               lv_event_cb_t click_cb = nullptr,
               void* user_data = nullptr,
               bool show_indicator = false);

    void build() override;
    /// Replaces the button label text.
    void set_text(const char* text);
    /// Changes the label font.
    void set_font(const lv_font_t* font);
    /// Enables or disables the button (also updates its styling).
    void set_enabled(bool enabled);

private:
    static void theme_observer_cb(lv_observer_t* observer, lv_subject_t* subject);

    void apply_theme(bool dark_mode);
    void apply_indicator_style();

    viewmodel::BaseViewModel& view_model_;
    int32_t width_;
    int32_t height_;
    std::string text_;
    const lv_font_t* font_;
    lv_color_t light_color_;
    lv_color_t dark_color_;
    lv_event_cb_t click_cb_;
    void* user_data_;
    bool show_indicator_{false};
    lv_obj_t* label_{nullptr};
    lv_obj_t* indicator_{nullptr};
    bool enabled_{true};

};

} // namespace view::widgets

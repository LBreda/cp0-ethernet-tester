/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_widget.h"

#include <cstdint>
#include <string>

namespace app {
class AssetManager;
}

namespace viewmodel {
class BaseViewModel;
}

namespace view::widgets {

/// Visual tone of a toast, mapped to theme status colors.
enum class ToastTone {
    Default,
    Success,
    Warning,
    Error,
};

/// Layout and timing parameters for a Toast (pixels / milliseconds).
struct ToastConfig {
    int32_t min_width{96};
    int32_t max_width{292};
    int32_t min_height{36};
    int32_t horizontal_padding{14};
    int32_t vertical_padding{8};
    int32_t bottom_offset{38};
    uint32_t duration_ms{2600};
};

/**
 * @brief Transient notification shown near the bottom of the screen.
 *
 * Supports auto-hiding and persistent messages, and an optional
 * highlighted middle segment (prefix + highlighted + suffix).
 */
class Toast : public BaseWidgets {
public:
    Toast(lv_obj_t* parent,
          viewmodel::BaseViewModel& view_model,
          app::AssetManager& assets,
          ToastConfig config = {});
    ~Toast() override;

    /// Creates the (initially hidden) toast container and label.
    void build() override;
    /**
     * @brief Shows a plain message that hides automatically.
     * @param duration_ms Display time; 0 uses ToastConfig::duration_ms.
     */
    void show(const std::string& message,
              ToastTone tone = ToastTone::Default,
              uint32_t duration_ms = 0);
    /**
     * @brief Shows a message whose middle part is highlighted.
     * @param duration_ms Display time; 0 uses ToastConfig::duration_ms.
     * @param scroll      Whether long text should scroll instead of wrapping.
     */
    void show_highlighted(const std::string& prefix,
                          const std::string& highlighted,
                          const std::string& suffix,
                          ToastTone tone = ToastTone::Default,
                          uint32_t duration_ms = 0,
                          bool scroll = false);
    /// Shows a message that stays visible until hide() is called.
    void show_persistent(const std::string& message, ToastTone tone = ToastTone::Default);
    /// Persistent variant of show_highlighted().
    void show_persistent_highlighted(const std::string& prefix,
                                     const std::string& highlighted,
                                     const std::string& suffix,
                                     ToastTone tone = ToastTone::Default);
    /// Hides the toast and cancels any pending hide timer.
    void hide();
    /// Returns true if the toast is currently shown.
    bool visible() const;

private:
    void show_current_message(uint32_t duration_ms, bool persistent);
    void update_label_text();
    void update_size();
    void apply_theme();
    static void hide_timer_cb(lv_timer_t* timer);
    static void theme_observer_cb(lv_observer_t* observer, lv_subject_t* subject);

    viewmodel::BaseViewModel& view_model_;
    app::AssetManager& assets_;
    ToastConfig config_;
    ToastTone tone_{ToastTone::Default};
    std::string prefix_;
    std::string highlighted_;
    std::string suffix_;
    bool scroll_{false};
    lv_obj_t* label_{nullptr};
    const lv_font_t* font_{nullptr};
    lv_timer_t* hide_timer_{nullptr};
};

} // namespace view::widgets

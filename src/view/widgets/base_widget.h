/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"

namespace view::widgets {

/**
 * @brief Base class for LVGL-backed widgets.
 *
 * Owns a single root LVGL object (`core_obj_`), which is deleted on
 * destruction if still valid. Subclasses create it in build().
 */
class BaseWidgets {
public:
    /// @param parent LVGL parent object the widget will be created under.
    explicit BaseWidgets(lv_obj_t* parent) : parent_(parent) {}
    virtual ~BaseWidgets() {
        if (core_obj_ && lv_obj_is_valid(core_obj_)) {
            lv_obj_delete(core_obj_);
        }
    }

    BaseWidgets(const BaseWidgets&) = delete;
    BaseWidgets& operator=(const BaseWidgets&) = delete;

    /// Creates the LVGL object tree for this widget.
    virtual void build() = 0;

    /// Returns the widget's root LVGL object (nullptr before build()).
    lv_obj_t* root() const { return core_obj_; }

protected:
    lv_obj_t* parent_{nullptr};
    lv_obj_t* core_obj_{nullptr};
};

} // namespace view::widgets

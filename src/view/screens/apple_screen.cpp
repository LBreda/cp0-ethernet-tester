/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "apple_screen.h"

#include "asset_manager.h"
#include "theme.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace screen {
namespace {

// Short tab captions, indexed by model::EthernetCategory.
constexpr std::array<const char*, model::kEthernetCategoryCount> kCategoryLabels = {
    "Summary", "Link", "Setup", "Ping", "Stats", "Peers", "mDNS", "Driver", "Switch",
    "DNS", "DHCP", "Listen"};
// Vertical layout (pixels) of the result card, its details viewport and the run button.
constexpr int kResultCardTop = 23;
constexpr int kResultCardBottomGap = 3;
constexpr int kRunButtonHeight = 19;
constexpr int kDetailsViewportTop = 35;
constexpr int kDetailsViewportBottomInset = 11;

const char* status_label(model::EthernetTestStatus status) {
    switch (status) {
        case model::EthernetTestStatus::Ready: return "READY";
        case model::EthernetTestStatus::Running: return "RUN";
        case model::EthernetTestStatus::Passed: return "PASS";
        case model::EthernetTestStatus::Failed: return "FAIL";
        case model::EthernetTestStatus::Unsupported: return "N/A";
        case model::EthernetTestStatus::Cancelled: return "STOP";
        case model::EthernetTestStatus::Error: return "ERROR";
    }
    return "INFO";
}

} // namespace

AppleScreen::AppleScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void AppleScreen::build_content(lv_obj_t* content) {
    tab_area_ = lv_obj_create(content);
    auto* tab_area = tab_area_;
    lv_obj_remove_style_all(tab_area);
    lv_obj_set_size(tab_area, 304, 21);
    lv_obj_align(tab_area, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(tab_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_area,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(tab_area, 0, 0);
    lv_obj_set_style_pad_column(tab_area, 3, 0);
    lv_obj_add_flag(tab_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_area, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tab_area, LV_SCROLLBAR_MODE_OFF);

    const auto* tab_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        auto* tab = lv_obj_create(tab_area);
        tabs_[index] = tab;
        tab_contexts_[index] = {&view_model(), static_cast<model::EthernetCategory>(index)};
        lv_obj_set_size(tab, 42, 19);
        lv_obj_set_style_radius(tab, 4, 0);
        lv_obj_set_style_border_width(tab, 1, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tab, tab_clicked_cb, LV_EVENT_CLICKED, &tab_contexts_[index]);

        auto* label = lv_label_create(tab);
        tab_labels_[index] = label;
        lv_label_set_text(label, kCategoryLabels[index]);
        lv_obj_set_style_text_font(label, tab_font ? tab_font : &lv_font_montserrat_12, 0);
        lv_obj_center(label);
    }

    // The result card fills the space between the tab strip and the run button.
    result_card_ = lv_obj_create(content);
    const int available_content_height = view::kScreenHeight - view::kTitleBarHeight;
    const int result_card_height = available_content_height - kResultCardTop - kRunButtonHeight -
                                   kResultCardBottomGap;
    lv_obj_set_size(result_card_, 304, result_card_height);
    lv_obj_align(result_card_, LV_ALIGN_TOP_MID, 0, kResultCardTop);
    lv_obj_set_style_radius(result_card_, 5, 0);
    lv_obj_set_style_border_width(result_card_, 1, 0);
    lv_obj_set_style_pad_all(result_card_, 5, 0);
    lv_obj_clear_flag(result_card_, LV_OBJ_FLAG_SCROLLABLE);

    summary_label_ = lv_label_create(result_card_);
    lv_label_bind_text(summary_label_, view_model().ethernet_summary_subject(), nullptr);
    lv_obj_set_width(summary_label_, 230);
    auto* summary_font = assets().load_standard_font(12);
    lv_obj_set_style_text_font(summary_label_, summary_font ? summary_font : &lv_font_montserrat_14, 0);
    lv_obj_align(summary_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    subtitle_label_ = lv_label_create(result_card_);
    lv_label_bind_text(subtitle_label_, view_model().ethernet_description_subject(), nullptr);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(subtitle_label_, 288);
    auto* subtitle_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(subtitle_label_, subtitle_font ? subtitle_font : &lv_font_montserrat_10, 0);
    lv_obj_align(subtitle_label_, LV_ALIGN_TOP_LEFT, 0, 15);

    status_label_ = lv_label_create(result_card_);
    lv_obj_set_size(status_label_, 50, 16);
    lv_obj_set_style_radius(status_label_, 3, 0);
    lv_obj_set_style_pad_hor(status_label_, 3, 0);
    lv_obj_set_style_pad_ver(status_label_, 1, 0);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    auto* status_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(status_label_, status_font ? status_font : &lv_font_montserrat_10, 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, 0, 0);

    details_viewport_ = lv_obj_create(result_card_);
    lv_obj_remove_style_all(details_viewport_);
    const int details_viewport_height =
        result_card_height - kDetailsViewportTop - kDetailsViewportBottomInset;
    lv_obj_set_size(details_viewport_, 288, details_viewport_height);
    lv_obj_align(details_viewport_, LV_ALIGN_TOP_LEFT, 0, kDetailsViewportTop);
    lv_obj_set_flex_flow(details_viewport_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(details_viewport_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(details_viewport_, 0, 0);
    lv_obj_set_style_pad_row(details_viewport_, 3, 0);
    lv_obj_add_flag(details_viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(details_viewport_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(details_viewport_, LV_SCROLLBAR_MODE_ACTIVE);

    run_button_ = lv_obj_create(content);
    lv_obj_set_size(run_button_, 112, kRunButtonHeight);
    lv_obj_align(run_button_, LV_ALIGN_BOTTOM_RIGHT, -8, 0);
    lv_obj_set_style_radius(run_button_, 4, 0);
    lv_obj_set_style_border_width(run_button_, 0, 0);
    lv_obj_set_style_pad_all(run_button_, 1, 0);
    lv_obj_clear_flag(run_button_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(run_button_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(run_button_, run_test_cb, LV_EVENT_CLICKED, &view_model());

    run_button_label_ = lv_label_create(run_button_);
    lv_obj_set_style_text_font(run_button_label_, tab_font ? tab_font : &lv_font_montserrat_12, 0);
    lv_obj_center(run_button_label_);

    scroll_hint_label_ = lv_label_create(content);
    lv_label_set_text(scroll_hint_label_, "UP / F  DOWN / X  SCROLL");
    auto* hint_font = assets().load_standard_font(9, app::StandardFontWeight::Regular);
    lv_obj_set_style_text_font(scroll_hint_label_, hint_font ? hint_font : &lv_font_montserrat_10, 0);
    lv_obj_align(scroll_hint_label_, LV_ALIGN_BOTTOM_LEFT, 8, -4);

    // All relevant subjects share one dispatcher; observers are tied to tab_area
    // so they are removed automatically when the screen is deleted.
    lv_subject_add_observer_obj(view_model().ethernet_category_subject(),
                                update_display_cb, tab_area, this);
    lv_subject_add_observer_obj(view_model().ethernet_status_subject(),
                                update_display_cb, tab_area, this);
    lv_subject_add_observer_obj(view_model().dark_mode_subject(),
                                update_display_cb, tab_area, this);
    lv_subject_add_observer_obj(view_model().ethernet_scroll_subject(),
                                update_display_cb, tab_area, this);
    lv_subject_add_observer_obj(view_model().ethernet_detail_subject(),
                                update_display_cb, tab_area, this);
    lv_subject_add_observer_obj(view_model().mdns_detail_mode_subject(),
                                update_display_cb, tab_area, this);
    update_detail_rows(lv_subject_get_string(view_model().ethernet_detail_subject()));
    update_display();
}

void AppleScreen::update_display() {
    const auto colors = view::palette(view_model().is_dark_mode());
    const auto selected = static_cast<std::size_t>(view_model().ethernet_category());
    const auto status = view_model().ethernet_test_status();
    const bool running = status == model::EthernetTestStatus::Running;

    for (std::size_t index = 0; index < tabs_.size(); ++index) {
        const bool active = index == selected;
        lv_obj_set_style_bg_color(tabs_[index], active ? colors.primary : colors.button, 0);
        lv_obj_set_style_bg_opa(tabs_[index], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tabs_[index], active ? colors.primary : colors.border, 0);
        lv_obj_set_style_text_color(tab_labels_[index], active ? colors.background : colors.text, 0);
        if (running) {
            lv_obj_add_state(tabs_[index], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(tabs_[index], LV_STATE_DISABLED);
        }
    }
    // Keep the selected tab visible in the horizontally scrolling strip.
    if (tab_area_ && selected < tabs_.size()) {
        lv_obj_update_layout(tab_area_);
        lv_obj_scroll_to_view(tabs_[selected], LV_ANIM_OFF);
    }

    lv_obj_set_style_bg_color(result_card_, colors.surface, 0);
    lv_obj_set_style_bg_opa(result_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(result_card_, colors.border, 0);
    auto status_color = colors.info;
    switch (status) {
        case model::EthernetTestStatus::Running: status_color = colors.info; break;
        case model::EthernetTestStatus::Passed: status_color = colors.success; break;
        case model::EthernetTestStatus::Failed:
        case model::EthernetTestStatus::Error: status_color = colors.error; break;
        case model::EthernetTestStatus::Unsupported:
        case model::EthernetTestStatus::Cancelled: status_color = colors.warning; break;
        case model::EthernetTestStatus::Ready: status_color = colors.text_disabled; break;
    }
    lv_obj_set_style_text_color(summary_label_, colors.text, 0);
    lv_label_set_text(status_label_, status_label(status));
    lv_obj_set_style_bg_color(status_label_, status_color, 0);
    lv_obj_set_style_bg_opa(status_label_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(status_label_, colors.background, 0);
    lv_obj_set_style_text_color(subtitle_label_, colors.text_disabled, 0);
    lv_obj_set_style_bg_color(run_button_, colors.primary, 0);
    lv_obj_set_style_bg_opa(run_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(run_button_label_, colors.background, 0);
    lv_obj_set_style_text_color(scroll_hint_label_, colors.text_disabled, 0);
    for (const auto& row : detail_rows_) {
        if (row.key) {
            auto color = colors.primary;
            // On Summary, color the block name by the health state in its value.
            if (view_model().ethernet_category() == model::EthernetCategory::Summary && row.value) {
                const std::string value = lv_label_get_text(row.value);
                if (value.find("GREEN") != std::string::npos) {
                    color = colors.success;
                } else if (value.find("YELLOW") != std::string::npos) {
                    color = colors.warning;
                } else if (value.find("RED") != std::string::npos) {
                    color = colors.error;
                } else if (value.find("NOT TESTED") != std::string::npos) {
                    color = colors.text_disabled;
                }
            }
            lv_obj_set_style_text_color(row.key, color, 0);
        }
        if (row.value) {
            lv_obj_set_style_text_color(row.value, colors.text, 0);
        }
    }
    if (view_model().ethernet_category() == model::EthernetCategory::Mdns) {
        lv_label_set_text(scroll_hint_label_, view_model().mdns_detail_mode()
                                                  ? "UP/DN SCROLL  ESC BACK"
                                                  : "UP/DN SELECT  TAB DETAILS");
    } else {
        lv_label_set_text(scroll_hint_label_, "UP / F  DOWN / X  SCROLL");
    }
    update_mdns_selection();
    const auto category = view_model().ethernet_category();
    lv_label_set_text(run_button_label_, running ? "CANCEL"
                                                 : (category == model::EthernetCategory::Summary
                                                        ? "AUTO TEST / ENTER"
                                                        : "RUN / ENTER"));
}

void AppleScreen::update_detail_rows(const char* detail) {
    if (!details_viewport_) {
        return;
    }
    detail_text_ = detail ? detail : "";
    if (view_model().ethernet_category() == model::EthernetCategory::Mdns) {
        update_mdns_content();
        return;
    }
    mdns_services_.clear();
    mdns_service_rows_.clear();
    update_standard_detail_rows(detail_text_);
}

// Rebuilds the details viewport from newline-separated text. "Key: value" lines
// become two-column rows; lines without ':' span the full width.
void AppleScreen::update_standard_detail_rows(const std::string& text) {
    if (!details_viewport_) {
        return;
    }
    lv_obj_clean(details_viewport_);
    detail_rows_.clear();
    mdns_service_rows_.clear();

    const auto* row_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    const auto* key_font = assets().load_standard_font(9);
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() : end + 1;
        if (line.empty()) {
            continue;
        }

        const auto separator = line.find(':');
        auto* row_obj = lv_obj_create(details_viewport_);
        lv_obj_remove_style_all(row_obj);
        lv_obj_set_size(row_obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(row_obj, 0, 0);
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* key_label = nullptr;
        lv_obj_t* value_label = nullptr;
        if (separator == std::string::npos) {
            value_label = lv_label_create(row_obj);
            lv_label_set_text(value_label, line.c_str());
            lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(value_label, 288);
            lv_obj_set_style_text_font(value_label, row_font ? row_font : &lv_font_montserrat_12, 0);
        } else {
            auto key = line.substr(0, separator);
            auto value = line.substr(separator + 1);
            while (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }

            key_label = lv_label_create(row_obj);
            lv_label_set_text(key_label, key.c_str());
            lv_label_set_long_mode(key_label, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(key_label, 90);
            lv_obj_set_style_text_font(key_label, key_font ? key_font : &lv_font_montserrat_10, 0);

            value_label = lv_label_create(row_obj);
            lv_label_set_text(value_label, value.c_str());
            lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(value_label, 198);
            lv_obj_set_style_text_font(value_label, row_font ? row_font : &lv_font_montserrat_12, 0);
        }

        detail_rows_.push_back({key_label, value_label});
    }

    if (detail_rows_.empty()) {
        auto* empty_label = lv_label_create(details_viewport_);
        lv_label_set_text(empty_label, "No details available.");
        lv_obj_set_style_text_font(empty_label,
                                   row_font ? row_font : &lv_font_montserrat_12,
                                   0);
    }
    lv_obj_update_layout(details_viewport_);
    lv_obj_scroll_to_y(details_viewport_, 0, LV_ANIM_OFF);
    update_display();
}

// Splits the mDNS browse result into an overview and per-service entries.
// A line containing " — " starts a new service; following lines are its
// details until an INCOMPLETE/stop notice returns to the overview.
void AppleScreen::update_mdns_content() {
    mdns_services_.clear();
    mdns_overview_.clear();
    selected_mdns_service_ = 0;
    MdnsServiceEntry* current_service = nullptr;
    std::size_t start = 0;
    while (start < detail_text_.size()) {
        const auto end = detail_text_.find('\n', start);
        const auto line = detail_text_.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? detail_text_.size() : end + 1;
        if (line.empty()) {
            continue;
        }
        if (line.find(" — ") != std::string::npos) {
            mdns_services_.push_back({line, {}});
            current_service = &mdns_services_.back();
            continue;
        }
        if (line.rfind("INCOMPLETE:", 0) == 0 ||
            line.rfind("The mDNS browse stopped", 0) == 0) {
            current_service = nullptr;
        }
        if (current_service) {
            current_service->details.push_back(line);
        } else {
            if (!mdns_overview_.empty()) {
                mdns_overview_.push_back('\n');
            }
            mdns_overview_ += line;
        }
    }
    if (view_model().mdns_detail_mode() && !mdns_services_.empty()) {
        render_mdns_detail();
    } else {
        render_mdns_list();
    }
}

// Renders overview lines followed by one selectable row per service. Summary
// lines duplicated by the list itself are hidden when services exist.
void AppleScreen::render_mdns_list() {
    if (!details_viewport_) {
        return;
    }
    lv_obj_clean(details_viewport_);
    detail_rows_.clear();
    mdns_service_rows_.clear();

    const auto* row_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    const auto* key_font = assets().load_standard_font(9);
    const auto is_redundant_summary_line = [](const std::string& line) {
        return line.rfind("mDNS services found (", 0) == 0 ||
               line.rfind("Browse duration:", 0) == 0 ||
               line.rfind("Interface:", 0) == 0;
    };
    const auto add_overview_line = [&](const std::string& line) {
        const bool full_width = line.rfind("INCOMPLETE:", 0) == 0 ||
                                line.rfind("Browse stopped:", 0) == 0 ||
                                line.rfind("The mDNS browse stopped", 0) == 0;
        const auto separator = full_width ? std::string::npos : line.find(':');
        auto* row_obj = lv_obj_create(details_viewport_);
        lv_obj_remove_style_all(row_obj);
        lv_obj_set_size(row_obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(row_obj, 0, 0);
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* key_label = nullptr;
        auto* value_label = lv_label_create(row_obj);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(value_label, row_font ? row_font : &lv_font_montserrat_12, 0);
        if (separator == std::string::npos) {
            lv_label_set_text(value_label, line.c_str());
            lv_obj_set_width(value_label, 288);
        } else {
            const auto key = line.substr(0, separator);
            auto value = line.substr(separator + 1);
            while (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }
            key_label = lv_label_create(row_obj);
            lv_label_set_text(key_label, key.c_str());
            lv_label_set_long_mode(key_label, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(key_label, 90);
            lv_obj_set_style_text_font(key_label, key_font ? key_font : &lv_font_montserrat_10, 0);
            lv_label_set_text(value_label, value.c_str());
            lv_obj_set_width(value_label, 198);
        }
        detail_rows_.push_back({key_label, value_label});
    };

    std::size_t start = 0;
    while (start < mdns_overview_.size()) {
        const auto end = mdns_overview_.find('\n', start);
        const auto line = mdns_overview_.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? mdns_overview_.size() : end + 1;
        if (!line.empty() &&
            (mdns_services_.empty() || !is_redundant_summary_line(line))) {
            add_overview_line(line);
        }
    }

    for (const auto& service : mdns_services_) {
        auto* row_obj = lv_obj_create(details_viewport_);
        lv_obj_remove_style_all(row_obj);
        lv_obj_set_size(row_obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row_obj, 3, 0);
        lv_obj_set_style_pad_hor(row_obj, 3, 0);
        lv_obj_set_style_pad_ver(row_obj, 2, 0);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);
        auto* label = lv_label_create(row_obj);
        lv_label_set_text(label, service.heading.c_str());
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, 278);
        lv_obj_set_style_text_font(label, row_font ? row_font : &lv_font_montserrat_12, 0);
        mdns_service_rows_.push_back(row_obj);
    }
    if (mdns_services_.empty() && mdns_overview_.empty()) {
        auto* empty_label = lv_label_create(details_viewport_);
        lv_label_set_text(empty_label, "No mDNS services found.");
        lv_obj_set_style_text_font(empty_label,
                                   row_font ? row_font : &lv_font_montserrat_12,
                                   0);
    }
    update_mdns_selection();
    lv_obj_update_layout(details_viewport_);
    if (selected_mdns_service_ < mdns_service_rows_.size()) {
        lv_obj_scroll_to_view(mdns_service_rows_[selected_mdns_service_], LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(details_viewport_, 0, LV_ANIM_OFF);
    }
    update_display();
}

// Renders the heading and detail lines of the selected service.
void AppleScreen::render_mdns_detail() {
    if (!details_viewport_ || mdns_services_.empty()) {
        return;
    }
    lv_obj_clean(details_viewport_);
    detail_rows_.clear();
    mdns_service_rows_.clear();

    const auto* row_font = assets().load_standard_font(10, app::StandardFontWeight::Regular);
    const auto* key_font = assets().load_standard_font(9);
    const auto add_line = [&](const std::string& line, bool full_width) {
        auto* row_obj = lv_obj_create(details_viewport_);
        lv_obj_remove_style_all(row_obj);
        lv_obj_set_size(row_obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(row_obj, 0, 0);
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* key_label = nullptr;
        auto* value_label = lv_label_create(row_obj);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(value_label, row_font ? row_font : &lv_font_montserrat_12, 0);
        const auto separator = full_width ? std::string::npos : line.find(':');
        if (separator == std::string::npos) {
            lv_label_set_text(value_label, line.c_str());
            lv_obj_set_width(value_label, 288);
        } else {
            const auto key = line.substr(0, separator);
            auto value = line.substr(separator + 1);
            while (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }
            key_label = lv_label_create(row_obj);
            lv_label_set_text(key_label, key.c_str());
            lv_label_set_long_mode(key_label, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(key_label, 90);
            lv_obj_set_style_text_font(key_label, key_font ? key_font : &lv_font_montserrat_10, 0);
            lv_label_set_text(value_label, value.c_str());
            lv_obj_set_width(value_label, 198);
        }
        detail_rows_.push_back({key_label, value_label});
    };

    const auto& service = mdns_services_[selected_mdns_service_];
    add_line(service.heading, true);
    for (const auto& line : service.details) {
        add_line(line, false);
    }
    if (service.details.empty()) {
        add_line("No service details available.", true);
    }
    lv_obj_update_layout(details_viewport_);
    lv_obj_scroll_to_y(details_viewport_, 0, LV_ANIM_OFF);
    update_display();
}

void AppleScreen::update_mdns_selection() {
    if (mdns_service_rows_.empty()) {
        return;
    }
    const auto colors = view::palette(view_model().is_dark_mode());
    for (std::size_t index = 0; index < mdns_service_rows_.size(); ++index) {
        auto* row = mdns_service_rows_[index];
        const bool selected = index == selected_mdns_service_;
        lv_obj_set_style_bg_color(row, selected ? colors.primary : colors.button, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(row, 0),
                                    selected ? colors.background : colors.text,
                                    0);
        if (selected) {
            lv_obj_add_state(row, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(row, LV_STATE_CHECKED);
        }
    }
}

// Moves the selection by delta, clamped to the list bounds (no wrap-around).
void AppleScreen::move_mdns_selection(int32_t delta) {
    if (mdns_service_rows_.empty() || delta == 0) {
        return;
    }
    const auto next = static_cast<int64_t>(selected_mdns_service_) + delta;
    const auto last = static_cast<int64_t>(mdns_service_rows_.size() - 1);
    const auto bounded = std::clamp<int64_t>(next, 0, last);
    if (static_cast<std::size_t>(bounded) == selected_mdns_service_) {
        return;
    }
    selected_mdns_service_ = static_cast<std::size_t>(bounded);
    update_mdns_selection();
    lv_obj_scroll_to_view(mdns_service_rows_[selected_mdns_service_], LV_ANIM_OFF);
}

void AppleScreen::tab_clicked_cb(lv_event_t* event) {
    auto* context = static_cast<TabContext*>(lv_event_get_user_data(event));
    if (context && context->view_model) {
        context->view_model->select_ethernet_category(context->category);
    }
}

void AppleScreen::run_test_cb(lv_event_t* event) {
    auto* view_model = static_cast<viewmodel::BaseViewModel*>(lv_event_get_user_data(event));
    if (!view_model) {
        return;
    }
    if (view_model->ethernet_test_status() == model::EthernetTestStatus::Running) {
        view_model->cancel_ethernet_test();
    } else {
        view_model->run_selected_ethernet_test();
    }
}

void AppleScreen::update_display_cb(lv_observer_t* observer, lv_subject_t* subject) {
    auto* screen = static_cast<AppleScreen*>(lv_observer_get_user_data(observer));
    if (!screen) {
        return;
    }
    // The scroll subject is a monotonically changing token; the difference from
    // the last seen value gives the number of up/down steps. In the mDNS list it
    // moves the selection, otherwise it scrolls the viewport by 24 px per step.
    if (subject == screen->view_model().ethernet_scroll_subject()) {
        const auto token = lv_subject_get_int(subject);
        const auto delta = token - screen->last_scroll_token_;
        screen->last_scroll_token_ = token;
        if (screen->view_model().ethernet_category() == model::EthernetCategory::Mdns &&
            !screen->view_model().mdns_detail_mode()) {
            screen->move_mdns_selection(delta);
        } else if (screen->details_viewport_ && delta != 0) {
            lv_obj_update_layout(screen->details_viewport_);
            const auto current = lv_obj_get_scroll_y(screen->details_viewport_);
            lv_obj_scroll_to_y(screen->details_viewport_, current + delta * 24, LV_ANIM_OFF);
        }
        return;
    }
    if (subject == screen->view_model().mdns_detail_mode_subject()) {
        if (screen->view_model().ethernet_category() == model::EthernetCategory::Mdns) {
            if (screen->view_model().mdns_detail_mode() && !screen->mdns_services_.empty()) {
                screen->render_mdns_detail();
            } else {
                screen->render_mdns_list();
            }
            screen->update_display();
        }
        return;
    }
    if (subject == screen->view_model().ethernet_detail_subject()) {
        screen->update_detail_rows(lv_subject_get_string(subject));
        return;
    }
    // Reset scroll position when switching category or when the status changes.
    if (screen->result_card_ &&
        (subject == screen->view_model().ethernet_category_subject() ||
         subject == screen->view_model().ethernet_status_subject())) {
        lv_obj_scroll_to_y(screen->details_viewport_, 0, LV_ANIM_OFF);
    }
    screen->update_display();
}

} // namespace screen

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <array>
#include <string>
#include <vector>

namespace screen {

/**
 * @brief Ethernet diagnostics screen.
 *
 * Shows one tab per Ethernet test category, a result card with key/value
 * detail rows, and a selectable mDNS service list/detail view.
 */
class AppleScreen : public BaseScreen {
public:
    AppleScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);

private:
    void build_content(lv_obj_t* content) override;
    void update_display();
    void update_detail_rows(const char* detail);
    void update_standard_detail_rows(const std::string& detail);
    void update_mdns_content();
    void render_mdns_list();
    void render_mdns_detail();
    void update_mdns_selection();
    void move_mdns_selection(int32_t delta);

    struct TabContext {
        viewmodel::BaseViewModel* view_model{nullptr};
        model::EthernetCategory category{model::EthernetCategory::Link};
    };

    static void tab_clicked_cb(lv_event_t* event);
    static void run_test_cb(lv_event_t* event);
    static void update_display_cb(lv_observer_t* observer, lv_subject_t* subject);

    struct DetailRow {
        lv_obj_t* key{nullptr};
        lv_obj_t* value{nullptr};
    };

    struct MdnsServiceEntry {
        std::string heading;
        std::vector<std::string> details;
    };

    std::array<lv_obj_t*, model::kEthernetCategoryCount> tabs_{};
    std::array<lv_obj_t*, model::kEthernetCategoryCount> tab_labels_{};
    std::array<TabContext, model::kEthernetCategoryCount> tab_contexts_{};
    lv_obj_t* tab_area_{nullptr};
    lv_obj_t* result_card_{nullptr};
    lv_obj_t* summary_label_{nullptr};
    lv_obj_t* subtitle_label_{nullptr};
    lv_obj_t* details_viewport_{nullptr};
    lv_obj_t* status_label_{nullptr};
    lv_obj_t* run_button_{nullptr};
    lv_obj_t* run_button_label_{nullptr};
    lv_obj_t* scroll_hint_label_{nullptr};
    std::vector<DetailRow> detail_rows_;
    std::vector<MdnsServiceEntry> mdns_services_;
    std::vector<lv_obj_t*> mdns_service_rows_;
    std::string mdns_overview_;
    std::string detail_text_;
    std::size_t selected_mdns_service_{0};
    int32_t last_scroll_token_{0};
};

} // namespace screen

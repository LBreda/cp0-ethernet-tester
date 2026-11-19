/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "apple_screen.h"
#include "asset_manager.h"
#include "base_viewmodel.h"
#include "theme.h"

#include <array>
#include <iostream>
#include <string>
#include <utility>

namespace {

// Prints a failure message when the condition is false; returns the condition.
bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// Depth-first search for an LVGL label whose text equals `text`.
lv_obj_t* find_label_with_text(lv_obj_t* parent, const std::string& text) {
    if (lv_obj_check_type(parent, &lv_label_class) &&
        text == lv_label_get_text(parent)) {
        return parent;
    }
    const auto child_count = lv_obj_get_child_count(parent);
    for (uint32_t index = 0; index < child_count; ++index) {
        if (auto* found = find_label_with_text(lv_obj_get_child(parent, index), text)) {
            return found;
        }
    }
    return nullptr;
}

// True if the object's vertical extent lies entirely within the viewport.
bool is_inside_viewport(lv_obj_t* object, lv_obj_t* viewport) {
    lv_area_t object_area;
    lv_area_t viewport_area;
    lv_obj_get_coords(object, &object_area);
    lv_obj_get_coords(viewport, &viewport_area);
    return object_area.y1 >= viewport_area.y1 && object_area.y2 <= viewport_area.y2;
}

// Finds the scrollable object with the most direct children (the detail list).
void find_largest_scrollable(lv_obj_t* object, lv_obj_t*& result, uint32_t& child_count) {
    const auto current_count = lv_obj_get_child_count(object);
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE) && current_count > child_count) {
        result = object;
        child_count = current_count;
    }
    for (uint32_t index = 0; index < current_count; ++index) {
        find_largest_scrollable(lv_obj_get_child(object, index), result, child_count);
    }
}

} // namespace

int main() {
    // Headless LVGL display matching the device resolution.
    lv_init();
    auto* display = lv_display_create(320, 170);
    if (!display) {
        std::cerr << "FAIL: headless LVGL display was created\n";
        lv_deinit();
        return 1;
    }

    bool passed = true;
    {
        viewmodel::BaseViewModel view_model;
        app::AssetManager assets;
        screen::AppleScreen screen(view_model, assets);
        lv_screen_load(screen.root());
        lv_obj_update_layout(screen.root());

        // Layout: title bar height, content area and result card geometry.
        auto* title_bar = lv_obj_get_child(screen.root(), 0);
        auto* content = lv_obj_get_child(screen.root(), 1);
        auto* result_card = content ? lv_obj_get_child(content, 1) : nullptr;
        auto* details_viewport = result_card ? lv_obj_get_child(result_card, 3) : nullptr;
        auto* run_button = content ? lv_obj_get_child(content, 2) : nullptr;
        passed &= check(view::kTitleBarHeight == 20 && title_bar &&
                            lv_obj_get_height(title_bar) == view::kTitleBarHeight,
                        "top bar is reduced from 30 to 20 pixels");
        passed &= check(content && lv_obj_get_y(content) == view::kTitleBarHeight &&
                            lv_obj_get_height(content) == view::kScreenHeight - view::kTitleBarHeight,
                        "main content starts below the shorter bar and gains 10 pixels");
        passed &= check(result_card && details_viewport && run_button &&
                            lv_obj_get_height(result_card) == 105 &&
                            lv_obj_get_height(details_viewport) == 59 &&
                            lv_obj_get_y(result_card) + lv_obj_get_height(result_card) + 3 ==
                                lv_obj_get_y(run_button),
                        "result card and scrolling viewport expand into the recovered space without overlapping controls");

        // Summary tab rendering.
        passed &= check(view_model.ethernet_category() == model::EthernetCategory::Summary,
                        "Summary is the first and initially selected category");
        const std::string summary_fixture =
            "Link: GREEN — updated 2 s ago\nIP / DNS: YELLOW — updated 8 s ago\n"
            "Listen: NOT TESTED\nNo announcements is not a network failure.";
        lv_subject_copy_string(view_model.ethernet_detail_subject(), summary_fixture.c_str());
        lv_obj_update_layout(screen.root());
        passed &= check(find_label_with_text(screen.root(), "Link") &&
                            find_label_with_text(screen.root(), "IP / DNS") &&
                            find_label_with_text(screen.root(), "Listen") &&
                            find_label_with_text(screen.root(), "GREEN — updated 2 s ago") &&
                            find_label_with_text(screen.root(), "YELLOW — updated 8 s ago") &&
                            find_label_with_text(screen.root(), "NOT TESTED"),
                        "Summary renders all three health blocks and their measurement ages");
        passed &= check(find_label_with_text(screen.root(), "AUTO TEST / ENTER"),
                        "Summary labels Enter as the explicit Auto Test action");

        // Discovery: subtitle, full-width lines, long details and scrolling.
        view_model.select_ethernet_category(model::EthernetCategory::Discovery);
        lv_obj_update_layout(screen.root());

        const auto description = std::string(lv_subject_get_string(view_model.ethernet_description_subject()));
        auto* subtitle = find_label_with_text(screen.root(), description);
        passed &= check(subtitle && lv_obj_get_width(subtitle) >= 280,
                        "category subtitle spans the content width instead of using an INFO column");
        passed &= check(description.find("press Enter") == std::string::npos,
                        "category subtitle does not repeat the run instruction");

        const std::string peer_summary = "IPv4 peers found (3)";
        lv_subject_copy_string(view_model.ethernet_detail_subject(), peer_summary.c_str());
        lv_obj_update_layout(screen.root());
        auto* peer_summary_label = find_label_with_text(screen.root(), peer_summary);
        passed &= check(peer_summary_label && lv_obj_get_width(peer_summary_label) >= 280,
                        "peer result count is rendered as a full-width line");

        std::string detail = "Interface: eth0\n";
        for (int index = 0; index < 80; ++index) {
            detail += "TX packets: " + std::to_string(index) + "\n";
        }
        passed &= check(detail.size() > 512, "scroll test detail exceeds the former 512-byte limit");
        lv_subject_copy_string(view_model.ethernet_detail_subject(), detail.c_str());
        passed &= check(std::string(lv_subject_get_string(view_model.ethernet_detail_subject())).size() == detail.size(),
                        "view-model detail subject preserves the complete result");
        lv_obj_update_layout(screen.root());

        auto* interface_label = find_label_with_text(screen.root(), "Interface");
        passed &= check(interface_label && lv_obj_get_width(interface_label) >= 90,
                        "result label column is wider than the previous 70 pixels");

        lv_obj_t* scrollable = nullptr;
        uint32_t scrollable_child_count = 0;
        find_largest_scrollable(screen.root(), scrollable, scrollable_child_count);
        passed &= check(scrollable && scrollable_child_count >= 80,
                        "long result rows are inside the scrollable details viewport");
        if (scrollable) {
            for (int index = 0; index < 12; ++index) {
                view_model.scroll_ethernet_results(1);
            }
            lv_obj_update_layout(screen.root());
            const auto down_position = lv_obj_get_scroll_y(scrollable);
            passed &= check(down_position > 0, "down key scrolls the long result toward later rows");

            for (int index = 0; index < 12; ++index) {
                view_model.scroll_ethernet_results(-1);
            }
            lv_obj_update_layout(screen.root());
            passed &= check(lv_obj_get_scroll_y(scrollable) < down_position,
                            "up key scrolls the result back toward earlier rows");
        }

        // Tab strip scrolling and per-category descriptions.
        view_model.select_ethernet_category(model::EthernetCategory::Listen);
        lv_obj_update_layout(screen.root());
        auto* listen_tab_label = find_label_with_text(screen.root(), "Listen");
        auto* tab_area = listen_tab_label
                             ? lv_obj_get_parent(lv_obj_get_parent(listen_tab_label))
                             : nullptr;
        passed &= check(listen_tab_label && tab_area && lv_obj_get_scroll_x(tab_area) > 0,
                        "selecting the last category scrolls its tab into view");

        const std::array<std::pair<model::EthernetCategory, const char*>, 3> added_tabs = {{
            {model::EthernetCategory::Summary, "Summary"},
            {model::EthernetCategory::Dhcp, "DHCP"},
            {model::EthernetCategory::Listen, "Listen"},
        }};
        for (const auto& tab : added_tabs) {
            view_model.select_ethernet_category(tab.first);
            lv_obj_update_layout(screen.root());
            passed &= check(find_label_with_text(screen.root(), tab.second) != nullptr,
                            "new design category is visible in the tab strip");
        }
        view_model.select_ethernet_category(model::EthernetCategory::Dhcp);
        passed &= check(std::string(lv_subject_get_string(view_model.ethernet_description_subject()))
                            .find("systemd-networkd lease") != std::string::npos,
                        "DHCP description states that the existing lease is read only");
        view_model.select_ethernet_category(model::EthernetCategory::Listen);
        passed &= check(std::string(lv_subject_get_string(view_model.ethernet_description_subject()))
                            .find("passive") != std::string::npos,
                        "Listen description identifies bounded passive observation");

        // mDNS: service list selection, detail view, back navigation and refresh.
        view_model.select_ethernet_category(model::EthernetCategory::Mdns);
        lv_obj_update_layout(screen.root());
        auto* mdns_tab_label = find_label_with_text(screen.root(), "mDNS");
        auto* mdns_tab = mdns_tab_label ? lv_obj_get_parent(mdns_tab_label) : nullptr;
        auto* mdns_tab_area = mdns_tab ? lv_obj_get_parent(mdns_tab) : nullptr;
        passed &= check(mdns_tab && mdns_tab_area && lv_obj_get_child(mdns_tab_area, 6) == mdns_tab,
                        "mDNS is the tab immediately after Peers");
        passed &= check(mdns_tab_label && mdns_tab_area && lv_obj_get_scroll_x(mdns_tab_area) > 0,
                        "selecting mDNS scrolls its tab into view");
        passed &= check(std::string(lv_subject_get_string(view_model.ethernet_description_subject()))
                            .find("DNS-SD") != std::string::npos,
                        "mDNS tab describes the DNS-SD browse");

        std::string mdns_detail = "mDNS services found (24)\nBrowse duration: 10 s\nInterface: eth0\n";
        for (int index = 0; index < 24; ++index) {
            const auto instance = "Printer " + std::to_string(index);
            mdns_detail += "_ipp._tcp — " + instance + "\n";
            mdns_detail += "Instance: " + instance + "\nType: _ipp._tcp\n";
            mdns_detail += "Hostname: printer-" + std::to_string(index) + ".local\n";
            mdns_detail += "Port: 631\nIPv4: 192.0.2." + std::to_string(index + 1) + "\n";
            for (int txt = 0; txt < 8; ++txt) {
                mdns_detail += "TXT: key" + std::to_string(txt) + "=value\n";
            }
        }
        mdns_detail += "\nINCOMPLETE: display budget reached; results may be partial.\n";
        lv_subject_copy_string(view_model.ethernet_detail_subject(), mdns_detail.c_str());
        lv_obj_update_layout(screen.root());
        auto* first_service_label = find_label_with_text(screen.root(), "_ipp._tcp — Printer 0");
        auto* last_service_label = find_label_with_text(screen.root(), "_ipp._tcp — Printer 23");
        auto* first_service_row = first_service_label ? lv_obj_get_parent(first_service_label) : nullptr;
        auto* last_service_row = last_service_label ? lv_obj_get_parent(last_service_label) : nullptr;
        auto* mdns_viewport = first_service_row ? lv_obj_get_parent(first_service_row) : nullptr;
        passed &= check(first_service_label && last_service_label &&
                            lv_obj_has_state(first_service_row, LV_STATE_CHECKED) &&
                            !lv_obj_has_state(last_service_row, LV_STATE_CHECKED),
                        "mDNS services are shown in a selectable list with the first item selected");
        auto* incomplete_warning = find_label_with_text(
            screen.root(), "INCOMPLETE: display budget reached; results may be partial.");
        passed &= check(mdns_viewport && incomplete_warning &&
                            lv_obj_get_width(incomplete_warning) >= 280 &&
                            !find_label_with_text(screen.root(), "mDNS services found (24)") &&
                            !find_label_with_text(screen.root(), "Browse duration: 10 s") &&
                            !find_label_with_text(screen.root(), "Interface: eth0"),
                        "mDNS list omits the summary while preserving the full-width incomplete-result warning");

        for (int index = 0; index < 23; ++index) {
            view_model.scroll_ethernet_results(1);
        }
        lv_obj_update_layout(screen.root());
        passed &= check(lv_obj_has_state(last_service_row, LV_STATE_CHECKED) &&
                            mdns_viewport && lv_obj_get_scroll_y(mdns_viewport) > 0,
                        "down key selects and scrolls the long mDNS service list");

        view_model.open_mdns_detail();
        lv_obj_update_layout(screen.root());
        passed &= check(view_model.mdns_detail_mode() &&
                            find_label_with_text(screen.root(), "printer-23.local") &&
                            find_label_with_text(screen.root(), "Port"),
                        "opening a selected service shows its resolved detail fields");
        auto* detail_label = find_label_with_text(screen.root(), "printer-23.local");
        auto* detail_viewport = detail_label
                                    ? lv_obj_get_parent(lv_obj_get_parent(detail_label))
                                    : nullptr;
        view_model.scroll_ethernet_results(1);
        view_model.scroll_ethernet_results(1);
        lv_obj_update_layout(screen.root());
        passed &= check(detail_viewport && lv_obj_get_scroll_y(detail_viewport) > 0,
                        "up/down scroll remains available within the selected service details");

        view_model.close_mdns_detail();
        lv_obj_update_layout(screen.root());
        last_service_label = find_label_with_text(screen.root(), "_ipp._tcp — Printer 23");
        last_service_row = last_service_label ? lv_obj_get_parent(last_service_label) : nullptr;
        passed &= check(!view_model.mdns_detail_mode() && last_service_row &&
                            lv_obj_has_state(last_service_row, LV_STATE_CHECKED) &&
                            mdns_viewport && lv_obj_get_scroll_y(mdns_viewport) > 0 &&
                            is_inside_viewport(last_service_row, mdns_viewport),
                        "back returns to the list with the selected service still visible");
        view_model.open_mdns_detail();
        lv_obj_update_layout(screen.root());
        passed &= check(view_model.mdns_detail_mode() &&
                            find_label_with_text(screen.root(), "printer-23.local"),
                        "the selected mDNS service can be reopened immediately after returning");
        view_model.close_mdns_detail();
        lv_obj_update_layout(screen.root());

        const std::string refreshed_mDNS =
            "mDNS services found (1)\nBrowse duration: 1 s\nInterface: eth0\n"
            "_ssh._tcp — Workstation\nInstance: Workstation\nType: _ssh._tcp\n"
            "Hostname: workstation.local\nPort: 22\nINCOMPLETE: query budget reached.\n";
        lv_subject_copy_string(view_model.ethernet_detail_subject(), refreshed_mDNS.c_str());
        lv_obj_update_layout(screen.root());
        const auto refreshed_heading = std::string("_ssh._tcp — Workstation");
        auto* refreshed_label = find_label_with_text(screen.root(), refreshed_heading);
        auto* refreshed_row = refreshed_label ? lv_obj_get_parent(refreshed_label) : nullptr;
        passed &= check(refreshed_row && lv_obj_has_state(refreshed_row, LV_STATE_CHECKED) &&
                            !find_label_with_text(screen.root(), "_ipp._tcp — Printer 23"),
                        "new mDNS results reset the list selection and discard stale rows");
    }

    lv_display_delete(display);
    lv_deinit();
    if (!passed) {
        return 1;
    }
    std::cout << "Ethernet screen layout and scrolling tests passed.\n";
    return 0;
}

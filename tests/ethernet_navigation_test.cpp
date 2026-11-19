/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_viewmodel.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// Compile-time checks on category count and tab ordering.
static_assert(model::kEthernetCategoryCount == 12);
static_assert(static_cast<int32_t>(model::EthernetCategory::Summary) == 0);
static_assert(static_cast<int32_t>(model::EthernetCategory::Listen) ==
              model::kEthernetCategoryCount - 1);
static_assert(static_cast<int32_t>(model::EthernetCategory::Mdns) ==
              static_cast<int32_t>(model::EthernetCategory::Discovery) + 1);

int main() {
    lv_init();
    bool passed = true;
    {
        // Synthetic runner records which categories Auto Test executes.
        std::mutex auto_test_mutex;
        std::vector<model::EthernetCategory> auto_test_categories;
        viewmodel::BaseViewModel view_model(
            [&auto_test_mutex, &auto_test_categories](model::EthernetCategory category,
                                                      const std::atomic<bool>&) {
                std::lock_guard<std::mutex> lock(auto_test_mutex);
                auto_test_categories.push_back(category);
                return model::EthernetTestResult{model::EthernetTestStatus::Passed,
                                                 "Synthetic test passed",
                                                 "No network operation was performed."};
            });
        // Scroll token increments/decrements.
        const auto initial_scroll = lv_subject_get_int(view_model.ethernet_scroll_subject());
        view_model.scroll_ethernet_results(1);
        passed &= lv_subject_get_int(view_model.ethernet_scroll_subject()) == initial_scroll + 1;
        view_model.scroll_ethernet_results(-1);
        passed &= lv_subject_get_int(view_model.ethernet_scroll_subject()) == initial_scroll;

        // Category navigation wraps around in both directions.
        passed &= view_model.is_dark_mode();
        view_model.select_ethernet_category(model::EthernetCategory::Summary);
        for (int32_t index = 1; index < model::kEthernetCategoryCount; ++index) {
            view_model.select_next_ethernet_category();
            passed &= lv_subject_get_int(view_model.ethernet_category_subject()) == index;
        }
        view_model.select_next_ethernet_category();
        passed &= view_model.ethernet_category() == model::EthernetCategory::Summary;
        view_model.select_previous_ethernet_category();
        passed &= view_model.ethernet_category() == model::EthernetCategory::Listen;
        view_model.select_ethernet_category(model::EthernetCategory::Discovery);
        view_model.select_next_ethernet_category();
        passed &= view_model.ethernet_category() == model::EthernetCategory::Mdns;
        view_model.select_next_ethernet_category();
        passed &= view_model.ethernet_category() == model::EthernetCategory::Advanced;

        // Auto Test from Summary runs the fixed sequence; LVGL timers are pumped
        // so results posted from the worker thread are applied.
        view_model.select_ethernet_category(model::EthernetCategory::Summary);
        view_model.run_selected_ethernet_test();
        const auto auto_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (view_model.ethernet_test_status() == model::EthernetTestStatus::Running &&
               std::chrono::steady_clock::now() < auto_deadline) {
            lv_tick_inc(10);
            lv_timer_handler();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {
            std::lock_guard<std::mutex> lock(auto_test_mutex);
            const std::vector<model::EthernetCategory> expected = {
                model::EthernetCategory::Link,
                model::EthernetCategory::Configuration,
                model::EthernetCategory::Connectivity,
                model::EthernetCategory::Dns,
                model::EthernetCategory::Neighbors,
            };
            passed &= auto_test_categories == expected;
        }
        passed &= view_model.ethernet_test_status() == model::EthernetTestStatus::Passed;
        passed &= view_model.ethernet_result(model::EthernetCategory::Link).has_completed_at;
        passed &= view_model.ethernet_result(model::EthernetCategory::Neighbors).has_completed_at;
        const auto health = view_model.ethernet_health_summary();
        passed &= health.blocks[0].state == model::EthernetHealthState::Green;
        passed &= health.blocks[1].state == model::EthernetHealthState::Green;
        passed &= health.blocks[2].state == model::EthernetHealthState::Green;
        passed &= std::string(lv_subject_get_string(view_model.ethernet_detail_subject()))
                      .find("No ARP, LLDP, DHCP or mDNS announcement is not a network failure") !=
                  std::string::npos;
        passed &= view_model.is_dark_mode();
    }

    {
        // Cancelling Auto Test during the first step stops the sequence.
        viewmodel::BaseViewModel view_model(
            [](model::EthernetCategory category, const std::atomic<bool>& cancel_requested) {
                if (category != model::EthernetCategory::Link) {
                    return model::EthernetTestResult{model::EthernetTestStatus::Error,
                                                     "Unexpected sequence",
                                                     "Cancellation should stop after the first step."};
                }
                while (!cancel_requested.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                return model::EthernetTestResult{model::EthernetTestStatus::Cancelled,
                                                 "Cancelled",
                                                 "Synthetic cancellation."};
            });
        view_model.select_ethernet_category(model::EthernetCategory::Summary);
        view_model.run_selected_ethernet_test();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        view_model.cancel_ethernet_test();
        const auto cancel_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (view_model.ethernet_test_status() == model::EthernetTestStatus::Running &&
               std::chrono::steady_clock::now() < cancel_deadline) {
            lv_tick_inc(10);
            lv_timer_handler();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        passed &= view_model.ethernet_test_status() == model::EthernetTestStatus::Cancelled;
        passed &= !view_model.ethernet_result(model::EthernetCategory::Configuration).has_completed_at;
    }
    lv_deinit();

    if (!passed) {
        std::cerr << "FAIL: Ethernet navigation, Auto Test, summary, scroll, or fixed dark theme\n";
        return 1;
    }
    std::cout << "Ethernet navigation tests passed.\n";
    return 0;
}
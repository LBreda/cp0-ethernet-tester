/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file base_viewmodel.h
 * @brief View model exposing application and Ethernet diagnostic state to LVGL screens via subjects.
 */

#pragma once

#include "subjects.h"
#include "base_model.h"
#include "ethernet_service.h"

#include "lvgl.h"

#include <array>
#include <cstddef>

namespace viewmodel {

/**
 * @brief Mediates between BaseModel / EthernetService and the UI.
 *
 * State is published through LVGL subjects that screens bind to; commands are
 * plain methods. Ethernet tests run on a worker thread and their results are
 * polled from an LVGL timer, so all subject updates happen on the UI thread.
 */
class BaseViewModel {
public:
    /** @param ethernet_runner Optional test runner override (e.g. for tests); defaults to the real diagnostics. */
    explicit BaseViewModel(platform::EthernetService::Runner ethernet_runner = {});
    ~BaseViewModel();

    BaseViewModel(const BaseViewModel&) = delete;
    BaseViewModel& operator=(const BaseViewModel&) = delete;

    /// @name Subjects for UI binding (owned by the view model)
    lv_subject_t* title_subject();
    lv_subject_t* greeting_subject();
    lv_subject_t* dark_mode_subject();
    lv_subject_t* current_page_subject();
    lv_subject_t* counter_subject();
    lv_subject_t* bold_text_subject();
    lv_subject_t* info_visible_subject();
    lv_subject_t* quit_requested_subject();
    lv_subject_t* ethernet_category_subject();
    lv_subject_t* ethernet_status_subject();
    lv_subject_t* ethernet_description_subject();
    lv_subject_t* ethernet_summary_subject();
    lv_subject_t* ethernet_detail_subject();
    lv_subject_t* ethernet_scroll_subject();
    lv_subject_t* mdns_detail_mode_subject();

    /// @name Queries and commands
    bool is_dark_mode() const;

    model::AppPage current_page() const;
    void show_apple_page();
    void show_butter_page();
    void toggle_page();
    void increment_counter();
    void decrement_counter();
    void toggle_bold_text();
    void toggle_info();
    /** @brief Sets the quit-requested subject; the main loop exits on it. */
    void request_quit();
    model::EthernetCategory ethernet_category() const;
    const model::EthernetTestResult& ethernet_result(model::EthernetCategory category) const;
    /** @brief Aggregated traffic-light health of the stored results. */
    model::EthernetHealthSummary ethernet_health_summary() const;
    void select_ethernet_category(model::EthernetCategory category);
    /** @brief Moves the selection to the previous/next category. */
    void select_previous_ethernet_category();
    void select_next_ethernet_category();
    /** @brief Scrolls the result detail; @p direction is negative for up, positive for down. */
    void scroll_ethernet_results(int32_t direction);
    bool mdns_detail_mode() const;
    /** @brief Enters/leaves the expanded mDNS service detail view. */
    void open_mdns_detail();
    void close_mdns_detail();
    model::EthernetTestStatus ethernet_test_status() const;
    /**
     * @brief Starts the selected test; on the Summary category runs the auto-test sequence instead.
     *
     * Does nothing while another test is running.
     */
    void run_selected_ethernet_test();
    /** @brief Requests cancellation of the running test (and of the auto-test sequence). */
    void cancel_ethernet_test();

private:
    void publish_all();
    void publish_ethernet_result();
    void publish_category_result();
    void publish_summary_detail();
    bool start_ethernet_operation(model::EthernetCategory category);
    void start_next_auto_test();
    void finish_auto_test(model::EthernetTestStatus status, const std::string& summary);
    static void poll_ethernet_result(lv_timer_t* timer);

    model::BaseModel model_;
    reactive::StringSubject<32> title_subject_;
    reactive::StringSubject<32> greeting_subject_;
    reactive::BoolSubject dark_mode_subject_;
    reactive::IntSubject current_page_subject_;
    reactive::IntSubject counter_subject_;
    reactive::BoolSubject bold_text_subject_;
    reactive::BoolSubject info_visible_subject_;
    reactive::BoolSubject quit_requested_subject_;
    reactive::IntSubject ethernet_category_subject_;
    reactive::IntSubject ethernet_status_subject_;
    reactive::StringSubject<128> ethernet_description_subject_;
    reactive::StringSubject<96> ethernet_summary_subject_;
    reactive::StringSubject<platform::EthernetService::kMaxDiscoveryDetailBytes + 1> ethernet_detail_subject_;
    reactive::IntSubject ethernet_scroll_subject_;
    reactive::BoolSubject mdns_detail_mode_subject_;
    lv_timer_t* ethernet_poll_timer_{nullptr};
    platform::EthernetService ethernet_service_;
    // Categories run in order when the Summary test is started.
    const std::array<model::EthernetCategory, 5> auto_test_sequence_{{
        model::EthernetCategory::Link,
        model::EthernetCategory::Configuration,
        model::EthernetCategory::Connectivity,
        model::EthernetCategory::Dns,
        model::EthernetCategory::Neighbors,
    }};
    std::size_t auto_test_index_{0};
    model::EthernetCategory active_ethernet_category_{model::EthernetCategory::Link};
    bool ethernet_operation_running_{false};
    bool auto_test_active_{false};
};

} // namespace viewmodel

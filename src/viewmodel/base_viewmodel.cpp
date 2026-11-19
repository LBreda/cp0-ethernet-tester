/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file base_viewmodel.cpp
 * @brief BaseViewModel implementation: subject publishing, test orchestration and result polling.
 */

#include "base_viewmodel.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace viewmodel {
namespace {

int page_to_int(model::AppPage page) {
    return static_cast<int>(page);
}

const char* category_name(model::EthernetCategory category) {
    switch (category) {
        case model::EthernetCategory::Summary: return "Field summary";
        case model::EthernetCategory::Link: return "Link diagnostics";
        case model::EthernetCategory::Configuration: return "Interface configuration";
        case model::EthernetCategory::Connectivity: return "Gateway connectivity";
        case model::EthernetCategory::Performance: return "Interface counters";
        case model::EthernetCategory::Discovery: return "Local network peers";
        case model::EthernetCategory::Mdns: return "mDNS DNS-SD services";
        case model::EthernetCategory::Advanced: return "Advanced diagnostics";
        case model::EthernetCategory::Neighbors: return "Switch neighbors";
        case model::EthernetCategory::Dns: return "Gateway DNS PTR";
        case model::EthernetCategory::Dhcp: return "Existing DHCP lease";
        case model::EthernetCategory::Listen: return "Passive LAN listen";
    }
    return "Ethernet diagnostics";
}

const char* category_description(model::EthernetCategory category) {
    switch (category) {
        case model::EthernetCategory::Summary:
            return "Health overview from completed tests; ENTER runs the field Auto Test.";
        case model::EthernetCategory::Link:
            return "Carrier, link state and negotiated speed.";
        case model::EthernetCategory::Configuration:
            return "Read-only IPv4, MAC address and gateway.";
        case model::EthernetCategory::Connectivity:
            return "One ping to eth0's configured gateway.";
        case model::EthernetCategory::Performance:
            return "Cumulative eth0 byte, packet and error counters; not throughput.";
        case model::EthernetCategory::Discovery:
            return "One bounded ARP scan of eth0's local IPv4 subnet.";
        case model::EthernetCategory::Mdns:
            return "Active 10 s DNS-SD browse on eth0; announced services only.";
        case model::EthernetCategory::Advanced:
            return "Driver details only; cable diagnostics are unsupported.";
        case model::EthernetCategory::Neighbors:
            return "Passive 5 s LLDP/CDP listen on eth0; requires CAP_NET_RAW or root.";
        case model::EthernetCategory::Dns:
            return "One PTR lookup for eth0's configured gateway via its IPv4 resolver.";
        case model::EthernetCategory::Dhcp:
            return "Read the existing systemd-networkd lease; sends no DHCP packets.";
        case model::EthernetCategory::Listen:
            return "Bounded passive ARP, DHCP and STP observation on eth0.";
    }
    return "No test is available for this category.";
}

} // namespace

BaseViewModel::BaseViewModel(platform::EthernetService::Runner ethernet_runner)
    : title_subject_(model_.app_title()),
      greeting_subject_(model_.greeting()),
      dark_mode_subject_(model_.dark_mode()),
      current_page_subject_(page_to_int(model_.current_page())),
      counter_subject_(0),
      bold_text_subject_(false),
      info_visible_subject_(false),
      quit_requested_subject_(false),
      ethernet_category_subject_(static_cast<int32_t>(model_.ethernet_category())),
      ethernet_status_subject_(static_cast<int32_t>(model_.ethernet_result().status)),
      ethernet_description_subject_(category_description(model_.ethernet_category())),
      ethernet_summary_subject_(model_.ethernet_result().summary.c_str()),
      ethernet_detail_subject_(model_.ethernet_result().detail.c_str()),
      mdns_detail_mode_subject_(false),
      ethernet_service_(std::move(ethernet_runner)) {
    ethernet_poll_timer_ = lv_timer_create(poll_ethernet_result, 100, this);
    publish_category_result();
}

BaseViewModel::~BaseViewModel() {
    if (ethernet_poll_timer_) {
        lv_timer_delete(ethernet_poll_timer_);
        ethernet_poll_timer_ = nullptr;
    }
    ethernet_service_.cancel();
}

lv_subject_t* BaseViewModel::title_subject() {
    return title_subject_.native();
}

lv_subject_t* BaseViewModel::greeting_subject() {
    return greeting_subject_.native();
}

lv_subject_t* BaseViewModel::dark_mode_subject() {
    return dark_mode_subject_.native();
}

lv_subject_t* BaseViewModel::current_page_subject() {
    return current_page_subject_.native();
}

lv_subject_t* BaseViewModel::counter_subject() {
    return counter_subject_.native();
}

lv_subject_t* BaseViewModel::bold_text_subject() {
    return bold_text_subject_.native();
}

lv_subject_t* BaseViewModel::info_visible_subject() {
    return info_visible_subject_.native();
}

lv_subject_t* BaseViewModel::quit_requested_subject() {
    return quit_requested_subject_.native();
}

lv_subject_t* BaseViewModel::ethernet_category_subject() {
    return ethernet_category_subject_.native();
}

lv_subject_t* BaseViewModel::ethernet_status_subject() {
    return ethernet_status_subject_.native();
}

lv_subject_t* BaseViewModel::ethernet_description_subject() {
    return ethernet_description_subject_.native();
}

lv_subject_t* BaseViewModel::ethernet_summary_subject() {
    return ethernet_summary_subject_.native();
}

lv_subject_t* BaseViewModel::ethernet_detail_subject() {
    return ethernet_detail_subject_.native();
}

lv_subject_t* BaseViewModel::ethernet_scroll_subject() {
    return ethernet_scroll_subject_.native();
}

lv_subject_t* BaseViewModel::mdns_detail_mode_subject() {
    return mdns_detail_mode_subject_.native();
}

bool BaseViewModel::is_dark_mode() const {
    return model_.dark_mode();
}

model::AppPage BaseViewModel::current_page() const {
    return model_.current_page();
}

void BaseViewModel::show_apple_page() {
    model_.set_current_page(model::AppPage::Apple);
    publish_all();
}

void BaseViewModel::show_butter_page() {
    model_.set_current_page(model::AppPage::Butter);
    publish_all();
}

void BaseViewModel::toggle_page() {
    model_.toggle_page();
    publish_all();
}

void BaseViewModel::increment_counter() {
    counter_subject_.set(counter_subject_.value() + 1);
}

void BaseViewModel::decrement_counter() {
    const auto next = counter_subject_.value() - 1;
    counter_subject_.set(next < 0 ? 0 : next);
}

void BaseViewModel::toggle_bold_text() {
    bold_text_subject_.toggle();
}

void BaseViewModel::toggle_info() {
    info_visible_subject_.toggle();
}

void BaseViewModel::request_quit() {
    quit_requested_subject_.set(true);
}

model::EthernetCategory BaseViewModel::ethernet_category() const {
    return model_.ethernet_category();
}

const model::EthernetTestResult& BaseViewModel::ethernet_result(
    model::EthernetCategory category) const {
    return model_.ethernet_result(category);
}

model::EthernetHealthSummary BaseViewModel::ethernet_health_summary() const {
    return model_.ethernet_health_summary();
}

void BaseViewModel::select_ethernet_category(model::EthernetCategory category) {
    if (ethernet_operation_running_) {
        return;
    }
    close_mdns_detail();
    model_.set_ethernet_category(category);
    ethernet_category_subject_.set(static_cast<int32_t>(category));
    ethernet_description_subject_.set(category_description(category));
    publish_category_result();
}

void BaseViewModel::select_previous_ethernet_category() {
    const auto current = static_cast<int32_t>(model_.ethernet_category());
    const auto previous = (current + model::kEthernetCategoryCount - 1) % model::kEthernetCategoryCount;
    select_ethernet_category(static_cast<model::EthernetCategory>(previous));
}

void BaseViewModel::select_next_ethernet_category() {
    const auto current = static_cast<int32_t>(model_.ethernet_category());
    select_ethernet_category(static_cast<model::EthernetCategory>(
        (current + 1) % model::kEthernetCategoryCount));
}

void BaseViewModel::scroll_ethernet_results(int32_t direction) {
    if (direction == 0) {
        return;
    }
    const auto step = direction < 0 ? -1 : 1;
    ethernet_scroll_subject_.set(ethernet_scroll_subject_.value() + step);
}

bool BaseViewModel::mdns_detail_mode() const {
    return mdns_detail_mode_subject_.value();
}

void BaseViewModel::open_mdns_detail() {
    if (model_.ethernet_category() != model::EthernetCategory::Mdns ||
        ethernet_operation_running_ ||
        std::string(lv_subject_get_string(ethernet_detail_subject_.native())).find(" — ") ==
            std::string::npos) {
        return;
    }
    mdns_detail_mode_subject_.set(true);
}

void BaseViewModel::close_mdns_detail() {
    mdns_detail_mode_subject_.set(false);
}

model::EthernetTestStatus BaseViewModel::ethernet_test_status() const {
    return ethernet_operation_running_ ? model::EthernetTestStatus::Running
                                       : model_.ethernet_result().status;
}

void BaseViewModel::run_selected_ethernet_test() {
    if (ethernet_operation_running_) {
        return;
    }
    close_mdns_detail();
    if (model_.ethernet_category() == model::EthernetCategory::Summary) {
        auto_test_index_ = 0;
        auto_test_active_ = true;
        start_next_auto_test();
        return;
    }
    start_ethernet_operation(model_.ethernet_category());
}

void BaseViewModel::cancel_ethernet_test() {
    if (ethernet_operation_running_) {
        ethernet_service_.cancel();
    }
}

void BaseViewModel::publish_ethernet_result() {
    const auto& result = model_.ethernet_result();
    ethernet_status_subject_.set(static_cast<int32_t>(result.status));
    ethernet_summary_subject_.set(result.summary.c_str());
    ethernet_detail_subject_.set(result.detail.c_str());
}

void BaseViewModel::publish_category_result() {
    const auto category = model_.ethernet_category();
    const auto& result = model_.ethernet_result(category);
    if (result.has_completed_at) {
        ethernet_status_subject_.set(static_cast<int32_t>(result.status));
        ethernet_summary_subject_.set(result.summary.c_str());
        ethernet_detail_subject_.set(result.detail.c_str());
        return;
    }
    ethernet_status_subject_.set(static_cast<int32_t>(model::EthernetTestStatus::Ready));
    ethernet_summary_subject_.set(category_name(category));
    if (category == model::EthernetCategory::Summary) {
        publish_summary_detail();
    } else {
        ethernet_detail_subject_.set("Press ENTER to run this diagnostic.");
    }
}

void BaseViewModel::publish_summary_detail() {
    const auto summary = model_.ethernet_health_summary();
    const auto now = std::chrono::system_clock::now();
    constexpr std::array<const char*, 3> kNames = {"Link", "IP / DNS", "Listen"};
    constexpr std::array<const char*, 4> kStates = {"NOT TESTED", "GREEN", "YELLOW", "RED"};
    std::string detail;
    for (std::size_t index = 0; index < summary.blocks.size(); ++index) {
        const auto& block = summary.blocks[index];
        detail += kNames[index];
        detail += ": ";
        detail += kStates[static_cast<std::size_t>(block.state)];
        if (block.stale) {
            detail += " (STALE)";
        }
        if (block.has_measurement) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - block.measured_at).count();
            const auto age = std::max<int64_t>(0, elapsed);
            detail += " — updated ";
            if (age < 60) {
                detail += std::to_string(age) + " s ago";
            } else if (age < 3600) {
                detail += std::to_string(age / 60) + " min ago";
            } else {
                detail += std::to_string(age / 3600) + " h ago";
            }
        }
        detail += '\n';
    }
    detail += "GREEN means the selected checks passed; it is not a cable certification.\n";
    detail += "No ARP, LLDP, DHCP or mDNS announcement is not a network failure.\n";
    detail += "Auto Test runs Link, Setup, Ping, DNS and Switch only when started.";
    ethernet_detail_subject_.set(detail.c_str());
}

bool BaseViewModel::start_ethernet_operation(model::EthernetCategory category) {
    if (!ethernet_service_.start(category)) {
        return false;
    }
    active_ethernet_category_ = category;
    ethernet_operation_running_ = true;
    ethernet_status_subject_.set(static_cast<int32_t>(model::EthernetTestStatus::Running));
    ethernet_summary_subject_.set(auto_test_active_
                                      ? ("Auto Test " + std::to_string(auto_test_index_ + 1) + "/" +
                                         std::to_string(auto_test_sequence_.size()) + ": " +
                                         category_name(category))
                                            .c_str()
                                      : "Test running");
    ethernet_detail_subject_.set("Checking eth0 without changing its configuration.");
    return true;
}

void BaseViewModel::start_next_auto_test() {
    if (auto_test_index_ >= auto_test_sequence_.size()) {
        finish_auto_test(model::EthernetTestStatus::Passed, "Auto Test complete");
        return;
    }
    if (!start_ethernet_operation(auto_test_sequence_[auto_test_index_])) {
        auto_test_active_ = false;
        ethernet_operation_running_ = false;
        finish_auto_test(model::EthernetTestStatus::Error, "Auto Test could not start");
    }
}

void BaseViewModel::finish_auto_test(model::EthernetTestStatus status,
                                    const std::string& summary) {
    auto_test_active_ = false;
    auto_test_index_ = 0;
    auto health = model_.ethernet_health_summary();
    bool has_red = false;
    bool has_yellow = false;
    for (const auto& block : health.blocks) {
        has_red |= block.state == model::EthernetHealthState::Red;
        has_yellow |= block.state == model::EthernetHealthState::Yellow;
    }
    if (status != model::EthernetTestStatus::Cancelled &&
        status != model::EthernetTestStatus::Error) {
        status = has_red ? model::EthernetTestStatus::Failed
                         : (has_yellow ? model::EthernetTestStatus::Unsupported
                                       : model::EthernetTestStatus::Passed);
    }
    std::string detail;
    const auto now = std::chrono::system_clock::now();
    constexpr std::array<const char*, 3> kNames = {"Link", "IP / DNS", "Listen"};
    constexpr std::array<const char*, 4> kStates = {"NOT TESTED", "GREEN", "YELLOW", "RED"};
    for (std::size_t index = 0; index < health.blocks.size(); ++index) {
        const auto& block = health.blocks[index];
        detail += kNames[index];
        detail += ": ";
        detail += kStates[static_cast<std::size_t>(block.state)];
        if (block.stale) {
            detail += " (STALE)";
        }
        if (block.has_measurement) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - block.measured_at).count();
            detail += " — updated " + std::to_string(std::max<int64_t>(0, elapsed)) + " s ago";
        }
        detail += '\n';
    }
    detail += "No ARP, LLDP, DHCP or mDNS announcement is not a network failure.\n";
    detail += "Other blocks are marked stale after an observed carrier change.\n";
    detail += "No settings were changed; DHCP was not renewed.";
    model_.set_ethernet_result(model::EthernetCategory::Summary,
                                {status, summary, std::move(detail)});
    publish_ethernet_result();
}

void BaseViewModel::poll_ethernet_result(lv_timer_t* timer) {
    auto* view_model = static_cast<BaseViewModel*>(lv_timer_get_user_data(timer));
    if (!view_model || !view_model->ethernet_operation_running_) {
        return;
    }

    auto result = view_model->ethernet_service_.result();
    if (result.status == model::EthernetTestStatus::Running) {
        return;
    }
    view_model->ethernet_operation_running_ = false;
    const auto category = view_model->active_ethernet_category_;
    const auto status = result.status;
    view_model->model_.set_ethernet_result(category, std::move(result));
    if (!view_model->auto_test_active_) {
        view_model->publish_ethernet_result();
        return;
    }
    if (status == model::EthernetTestStatus::Cancelled) {
        view_model->finish_auto_test(status, "Auto Test cancelled");
        return;
    }
    ++view_model->auto_test_index_;
    if (view_model->auto_test_index_ < view_model->auto_test_sequence_.size()) {
        view_model->start_next_auto_test();
        return;
    }
    view_model->finish_auto_test(model::EthernetTestStatus::Passed, "Auto Test complete");
}

void BaseViewModel::publish_all() {
    title_subject_.set(model_.app_title());
    greeting_subject_.set(model_.greeting());
    dark_mode_subject_.set(model_.dark_mode());
    current_page_subject_.set(page_to_int(model_.current_page()));
}

} // namespace viewmodel

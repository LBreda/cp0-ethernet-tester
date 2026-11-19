/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file base_model.cpp
 * @brief BaseModel implementation.
 */

#include "base_model.h"

#include <utility>

namespace model {

const char* BaseModel::app_title() const {
    return "Ethernet Tester";
}

const char* BaseModel::greeting() const {
    return "Local eth0 diagnostics";
}

bool BaseModel::dark_mode() const {
    return dark_mode_;
}

AppPage BaseModel::current_page() const {
    return current_page_;
}


void BaseModel::set_current_page(AppPage page) {
    current_page_ = page;
}

void BaseModel::toggle_page() {
    current_page_ = current_page_ == AppPage::Apple ? AppPage::Butter : AppPage::Apple;
}

EthernetCategory BaseModel::ethernet_category() const {
    return ethernet_model_.category();
}

void BaseModel::set_ethernet_category(EthernetCategory category) {
    ethernet_model_.set_category(category);
}

const EthernetTestResult& BaseModel::ethernet_result() const {
    return ethernet_model_.result();
}

const EthernetTestResult& BaseModel::ethernet_result(EthernetCategory category) const {
    return ethernet_model_.result(category);
}

void BaseModel::set_ethernet_result(EthernetTestResult result) {
    ethernet_model_.set_result(std::move(result));
}

void BaseModel::set_ethernet_result(EthernetCategory category, EthernetTestResult result) {
    ethernet_model_.set_result(category, std::move(result));
}

EthernetHealthSummary BaseModel::ethernet_health_summary() const {
    return ethernet_model_.health_summary();
}

} // namespace model

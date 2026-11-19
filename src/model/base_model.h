/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file base_model.h
 * @brief Application-wide model: UI page state, theme and Ethernet diagnostics.
 */

#pragma once

#include "ethernet_model.h"

namespace model {

/** @brief Top-level pages the application can display. */
enum class AppPage {
    Apple  = 0,
    Butter = 1,
};

/** @brief Plain application state, free of any UI dependency. */
class BaseModel {
public:
    /** @brief Static application title text. */
    const char* app_title() const;
    /** @brief Static greeting text. */
    const char* greeting() const;

    /** @brief Initial dark-mode preference. */
    bool dark_mode() const;

    AppPage current_page() const;
    void set_current_page(AppPage page);
    /** @brief Switches between the Apple and Butter pages. */
    void toggle_page();

    /// Ethernet diagnostics state; forwards to the embedded EthernetModel.
    EthernetCategory ethernet_category() const;
    void set_ethernet_category(EthernetCategory category);
    const EthernetTestResult& ethernet_result() const;
    const EthernetTestResult& ethernet_result(EthernetCategory category) const;
    void set_ethernet_result(EthernetTestResult result);
    void set_ethernet_result(EthernetCategory category, EthernetTestResult result);
    EthernetHealthSummary ethernet_health_summary() const;

private:
    bool dark_mode_ = true;
    AppPage current_page_ = AppPage::Apple;
    EthernetModel ethernet_model_;
};

} // namespace model

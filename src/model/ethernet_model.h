/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ethernet_model.h
 * @brief Data types and state for Ethernet diagnostic tests and their health summary.
 */

#pragma once

#include <cstdint>
#include <array>
#include <chrono>
#include <string>

namespace model {

/** @brief Diagnostic categories; values index result storage and UI lists. */
enum class EthernetCategory : int32_t {
    Summary = 0,
    Link = 1,
    Configuration = 2,
    Connectivity = 3,
    Performance = 4,
    Discovery = 5,
    Mdns = 6,
    Advanced = 7,
    Neighbors = 8,
    Dns = 9,
    Dhcp = 10,
    Listen = 11,
};

/** @brief Number of EthernetCategory values. */
constexpr int32_t kEthernetCategoryCount = 12;

/** @brief Traffic-light health of a group of tests. */
enum class EthernetHealthState : int32_t {
    NotTested = 0,
    Green = 1,
    Yellow = 2,
    Red = 3,
};

/**
 * @brief Aggregated health of one group of categories.
 *
 * measured_at is the newest completion time in the group; stale is set when a
 * later link flap may have invalidated the measurement.
 */
struct EthernetHealthBlock {
    EthernetHealthState state{EthernetHealthState::NotTested};
    std::chrono::system_clock::time_point measured_at{};
    bool has_measurement{false};
    bool stale{false};
};

/** @brief Health of the three groups: link, network configuration/reachability, and neighbors/discovery. */
struct EthernetHealthSummary {
    std::array<EthernetHealthBlock, 3> blocks{};
};

/** @brief Lifecycle/outcome of a single diagnostic test. */
enum class EthernetTestStatus : int32_t {
    Ready = 0,
    Running = 1,
    Passed = 2,
    Failed = 3,
    Unsupported = 4,
    Cancelled = 5,
    Error = 6,
};

/** @brief Outcome of one diagnostic run, with display text and completion time. */
struct EthernetTestResult {
    EthernetTestStatus status{EthernetTestStatus::Ready};
    std::string summary{"Link diagnostics"};
    std::string detail{"Press ENTER to run this diagnostic."};
    std::chrono::system_clock::time_point completed_at{};
    bool has_completed_at{false};
};

/**
 * @brief Stores the selected category and the latest result of every category.
 *
 * Overloads without a category argument operate on the selected category.
 */
class EthernetModel {
public:
    EthernetCategory category() const;
    void set_category(EthernetCategory category);

    const EthernetTestResult& result() const;
    void set_result(EthernetTestResult result);
    const EthernetTestResult& result(EthernetCategory category) const;
    /** @brief Stores @p result, stamping the completion time for finished statuses if missing. */
    void set_result(EthernetCategory category, EthernetTestResult result);
    /** @brief Computes per-group health from the stored results. */
    EthernetHealthSummary health_summary() const;

private:
    EthernetCategory category_{EthernetCategory::Summary};
    std::array<EthernetTestResult, kEthernetCategoryCount> results_{};
};

} // namespace model
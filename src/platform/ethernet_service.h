/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/// @file ethernet_service.h
/// @brief Runs Ethernet tests asynchronously on a worker thread.

#pragma once

#include "ethernet_model.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace platform {

/// Runs one Ethernet test category at a time in the background and exposes its latest result.
class EthernetService {
public:
    /// Test implementation; must poll the cancel flag. Empty means the built-in runner.
    using Runner = std::function<model::EthernetTestResult(
        model::EthernetCategory, const std::atomic<bool>&)>;

    explicit EthernetService(Runner runner = {});
    ~EthernetService();

    EthernetService(const EthernetService&) = delete;
    EthernetService& operator=(const EthernetService&) = delete;

    /// Starts a test; returns false if one is already running.
    bool start(model::EthernetCategory category);
    /// Requests cancellation of the running test.
    void cancel();
    /// Thread-safe copy of the current or last result.
    model::EthernetTestResult result() const;
    /// Reads the current DHCP lease of the Ethernet interface.
    static model::EthernetTestResult read_dhcp_lease();
    /// Passively observes ARP/DHCP/STP traffic until done or cancelled.
    static model::EthernetTestResult listen_for_field_events(const std::atomic<bool>& cancel_requested);

    /// Helpers for validating input and building/parsing external tool invocations.
    static bool is_valid_ipv4_address(const std::string& address);
    static std::vector<std::string> gateway_ping_arguments(const std::string& gateway);
    static std::vector<std::string> discovery_scan_arguments();
    /// Extracts (address, description) pairs from discovery scan output.
    static std::vector<std::pair<std::string, std::string>> parse_discovery_scan_output(
        const std::string& output);

    static constexpr std::size_t kMaxDiscoveryDetailBytes = 16 * 1024;

private:
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> cancel_requested_{false};
    model::EthernetTestResult result_{};
    Runner runner_;
};

} // namespace platform
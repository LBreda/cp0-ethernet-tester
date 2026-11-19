/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/// @file ethernet_diagnostics.h
/// @brief Ethernet diagnostics: LLDP/CDP neighbor parsing, gateway reverse-DNS lookup and link monitoring.

#pragma once

#include "ethernet_model.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace platform::diagnostics {

/// Switch/neighbor information decoded from an LLDP or CDP frame.
struct NeighborAdvertisement {
    std::string protocol;
    std::string chassis_id;
    std::string port_id;
    std::string system_name;
    std::string port_description;
    std::string vlan;
};

/// Result of parsing a DNS PTR (reverse lookup) response.
struct DnsPtrResponse {
    bool valid{false};
    bool truncated{false};
    uint8_t response_code{0};
    std::vector<std::string> ptr_records;
};

/// Difference between two counter samples; `reset` is set when the counter went backwards.
struct CounterDelta {
    uint64_t value{0};
    bool reset{false};
};

/// Parses a raw LLDP Ethernet frame; std::nullopt if malformed or not LLDP.
std::optional<NeighborAdvertisement> parse_lldp_frame(const std::vector<uint8_t>& frame);
/// Parses a raw Cisco Discovery Protocol frame; std::nullopt if malformed or not CDP.
std::optional<NeighborAdvertisement> parse_cdp_frame(const std::vector<uint8_t>& frame);
/// Tries both LLDP and CDP parsing on the given frame.
std::optional<NeighborAdvertisement> parse_neighbor_frame(const std::vector<uint8_t>& frame);

/// Builds the in-addr.arpa name for an IPv4 gateway; std::nullopt if the address is invalid.
std::optional<std::string> gateway_ptr_query_name(const std::string& gateway);
/// Encodes a DNS PTR query packet for the gateway address.
std::vector<uint8_t> make_ptr_query(const std::string& gateway, uint16_t transaction_id);
/// Validates and decodes a DNS PTR response matching the transaction id and query name.
DnsPtrResponse parse_ptr_response(const std::vector<uint8_t>& packet,
                                  uint16_t transaction_id,
                                  const std::string& expected_query_name);

/// Computes `after - before`, flagging counter resets.
CounterDelta counter_delta(uint64_t before, uint64_t after);

/// Diagnostic runners below block until finished or `cancel_requested` becomes true.
/// Monitors link state and interface error counters.
model::EthernetTestResult monitor_link(const std::atomic<bool>& cancel_requested);
/// Listens for LLDP/CDP advertisements from the attached switch.
model::EthernetTestResult listen_for_neighbors(const std::atomic<bool>& cancel_requested);
/// Performs a reverse DNS lookup of the default gateway.
model::EthernetTestResult lookup_gateway_ptr(const std::atomic<bool>& cancel_requested);

} // namespace platform::diagnostics

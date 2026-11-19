/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/// @file ethernet_field.h
/// @brief DHCP lease reading and passive observation of ARP, DHCP offer and STP traffic.

#pragma once

#include "ethernet_model.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace platform::field {

/// Maximum lease file size that will be read.
constexpr std::size_t kMaxLeaseBytes = 16 * 1024;
/// Maximum number of frames recorded per field capture.
constexpr std::size_t kMaxObservedFrames = 64;

/// DHCP lease details; timer fields are empty when not provided by the lease source.
struct DhcpLease {
    std::string address;
    std::string router;
    std::string server_address;
    std::vector<std::string> dns_servers;
    std::optional<uint32_t> lifetime_seconds;
    std::optional<uint32_t> renewal_seconds;
    std::optional<uint32_t> rebinding_seconds;
};

/// Kind of passively observed frame.
enum class Protocol : uint8_t {
    Arp,
    DhcpOffer,
    Stp,
};

/// Fields extracted from one observed frame; only fields relevant to `protocol` are set.
struct PacketObservation {
    Protocol protocol{Protocol::Arp};
    std::string source_mac;
    std::string ipv4;
    std::string related_ipv4;
    std::string server_ipv4;
    std::string offered_ipv4;
    uint8_t bpdu_version{0};
    uint8_t bpdu_type{0};
};

/// An IPv4 address and the MAC addresses seen claiming it.
struct AddressObservation {
    std::string address;
    std::vector<std::string> mac_addresses;
};

/// Aggregated capture results; `incomplete` marks truncated or aborted captures.
struct FieldSummary {
    std::size_t frames_recorded{0};
    std::size_t arp_frames{0};
    std::size_t dhcp_offer_frames{0};
    std::size_t stp_frames{0};
    std::vector<AddressObservation> arp_senders;
    std::vector<std::string> possible_ip_conflicts;
    std::vector<std::string> dhcp_servers;
    std::vector<std::string> stp_bpdus;
    bool incomplete{false};
};

/// Parses a systemd-networkd lease file (KEY=VALUE format).
std::optional<DhcpLease> parse_networkd_lease(const std::string& contents);
/// Parses a NetworkManager (dhclient-style) lease, selecting the entry matching interface/address.
std::optional<DhcpLease> parse_networkmanager_lease(const std::string& contents,
                                                   const std::string& interface_name,
                                                   const std::string& current_address);
/// Reads and parses the systemd-networkd lease for an interface.
std::optional<DhcpLease> read_systemd_networkd_lease(const std::string& interface_name);
/// Reads and parses the NetworkManager lease for an interface.
std::optional<DhcpLease> read_networkmanager_lease(const std::string& interface_name,
                                                   const std::string& current_address);
/// Classifies a raw Ethernet frame as ARP, DHCP offer or STP BPDU; std::nullopt otherwise.
std::optional<PacketObservation> parse_ethernet_frame(const std::vector<uint8_t>& frame);

/// Aggregates packet observations into a FieldSummary (counts, IP conflicts, servers, BPDUs).
class ObservationAccumulator {
public:
    /// Adds one observation to the running totals.
    void ingest(const PacketObservation& observation);
    /// Flags the capture as incomplete (e.g. cancelled or frame limit reached).
    void mark_incomplete();
    /// Returns the aggregated summary.
    FieldSummary summary() const;

private:
    std::size_t frames_recorded_{0};
    std::size_t arp_frames_{0};
    std::size_t dhcp_offer_frames_{0};
    std::size_t stp_frames_{0};
    std::map<std::string, std::set<std::string>> arp_macs_;
    std::set<std::string> dhcp_servers_;
    std::set<std::string> stp_bpdus_;
    bool incomplete_{false};
};

/// Passively captures traffic on the Ethernet interface until done or cancelled.
model::EthernetTestResult listen_for_field_events(const std::atomic<bool>& cancel_requested);

} // namespace platform::field

/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/// @file ethernet_mdns.h
/// @brief Bounded mDNS/DNS-SD packet parser, TTL record cache and service browser.

#pragma once

#include "ethernet_model.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace platform::mdns {

/// Resource limits protecting parser, cache and browse session against oversized or hostile traffic.
constexpr std::size_t kMaxPacketBytes = 9000;
constexpr std::size_t kMaxQuestionsPerPacket = 16;
constexpr std::size_t kMaxRecordsPerPacket = 128;
constexpr std::size_t kMaxCachedRecords = 512;
constexpr std::size_t kMaxCacheBytes = 128 * 1024;
constexpr std::size_t kMaxServiceInstances = 128;
constexpr std::size_t kMaxTxtEntries = 32;
constexpr std::size_t kMaxTxtBytes = 1024;
constexpr uint32_t kMaxRecordTtlSeconds = 24 * 60 * 60;
constexpr std::size_t kMaxBrowseQueries = 64;
constexpr std::size_t kMaxBrowsePackets = 256;
constexpr std::size_t kMaxBrowseDetailBytes = 16 * 1024;
constexpr auto kBrowseDuration = std::chrono::seconds(10);

/// Supported DNS record types (values are wire-format type codes).
enum class RecordType : uint16_t {
    A = 1,
    Ptr = 12,
    Txt = 16,
    Aaaa = 28,
    Srv = 33,
};

/// A decoded resource record; type-specific fields are filled according to `type`.
struct ResourceRecord {
    std::string owner;
    RecordType type{RecordType::A};
    uint32_t ttl{0};
    bool cache_flush{false};
    std::string target;
    uint16_t priority{0};
    uint16_t weight{0};
    uint16_t port{0};
    std::vector<std::string> text;
    std::string address;
};

/// Result of parsing one mDNS packet; `incomplete` means records were skipped due to limits.
struct ParsedPacket {
    bool valid{false};
    bool truncated{false};
    bool incomplete{false};
    std::string error;
    std::vector<ResourceRecord> records;
};

/// Parses an mDNS packet, keeping records of supported types.
ParsedPacket parse_packet(const std::vector<uint8_t>& packet);
/// Encodes a single-question mDNS query.
std::vector<uint8_t> make_query_packet(const std::string& name, RecordType type);

/// A DNS-SD service instance resolved from PTR/SRV/TXT/A/AAAA records.
struct ServiceInstance {
    std::string service_type;
    std::string instance;
    std::string instance_fqdn;
    std::string hostname;
    std::optional<uint16_t> port;
    std::vector<std::string> addresses;
    std::vector<std::string> text;
};

/// TTL-aware record cache that resolves DNS-SD service instances.
class ServiceCache {
public:
    using Clock = std::chrono::steady_clock;

    /// Stores records from a packet, honouring cache-flush, TTL and size limits.
    void ingest(const ParsedPacket& packet, Clock::time_point now = Clock::now());
    /// Drops records whose TTL has elapsed.
    void expire(Clock::time_point now = Clock::now());
    /// Returns discovered service types.
    std::vector<std::string> service_types(Clock::time_point now = Clock::now());
    /// Returns service instances resolved from cached records.
    std::vector<ServiceInstance> services(Clock::time_point now = Clock::now());

    /// True if records were dropped because of cache limits.
    bool incomplete() const;
    std::size_t record_count() const;
    std::size_t cache_bytes() const;

private:
    struct Entry {
        ResourceRecord record;
        Clock::time_point received_at;
        Clock::time_point expires_at;
        std::size_t bytes{0};
    };

    std::vector<Entry> records_;
    std::size_t cache_bytes_{0};
    bool incomplete_{false};
};

/// Outcome of BrowseTransport::receive().
enum class ReceiveStatus {
    Packet,
    Timeout,
    Error,
};

/// Network abstraction for mDNS browsing (replaceable in tests).
class BrowseTransport {
public:
    virtual ~BrowseTransport() = default;
    virtual bool open(std::string& error) = 0;
    virtual bool send_query(const std::string& name, RecordType type, std::string& error) = 0;
    virtual ReceiveStatus receive(std::chrono::milliseconds timeout,
                                  std::vector<uint8_t>& packet,
                                  std::string& error) = 0;
    virtual void close() = 0;
};

/// Time source used by browse_mdns (injectable for tests).
using BrowseClock = std::function<ServiceCache::Clock::time_point()>;

/// Browses mDNS services on the Ethernet interface using the default multicast transport.
model::EthernetTestResult browse_mdns(const std::atomic<bool>& cancel_requested);
/// Browses mDNS services for `duration` using the given transport and clock.
model::EthernetTestResult browse_mdns(const std::atomic<bool>& cancel_requested,
                                     BrowseTransport& transport,
                                     BrowseClock now,
                                     std::chrono::milliseconds duration = kBrowseDuration);

} // namespace platform::mdns

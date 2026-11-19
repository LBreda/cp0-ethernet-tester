/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ethernet_mdns.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using platform::mdns::RecordType;

// Prints a failure message when the condition is false; returns the condition.
bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// Big-endian (network byte order) integer writers.
void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
    bytes.push_back(static_cast<uint8_t>(value & 0xffU));
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 24U));
    bytes.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<uint8_t>(value & 0xffU));
}

// Splits a dotted DNS name into its labels.
std::vector<std::string> split_name(const std::string& name) {
    std::vector<std::string> labels;
    std::size_t start = 0;
    while (start < name.size()) {
        const auto end = name.find('.', start);
        labels.push_back(name.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return labels;
}

// Test-side description of a DNS resource record before wire encoding.
struct WireRecord {
    std::string owner;
    uint16_t type;
    uint32_t ttl;
    bool cache_flush;
    std::vector<uint8_t> data;
};

// Encodes an uncompressed DNS name: <len><label>... terminated by a 0 byte.
std::vector<uint8_t> encode_name(const std::string& name) {
    std::vector<uint8_t> result;
    for (const auto& label : split_name(name)) {
        result.push_back(static_cast<uint8_t>(label.size()));
        result.insert(result.end(), label.begin(), label.end());
    }
    result.push_back(0);
    return result;
}

std::vector<uint8_t> name_rdata(const std::string& name) {
    return encode_name(name);
}

// SRV RDATA: priority(16) | weight(16) | port(16) | target name.
std::vector<uint8_t> srv_rdata(uint16_t port, const std::string& target) {
    std::vector<uint8_t> result;
    append_u16(result, 0);
    append_u16(result, 0);
    append_u16(result, port);
    const auto encoded = encode_name(target);
    result.insert(result.end(), encoded.begin(), encoded.end());
    return result;
}

// TXT RDATA: sequence of length-prefixed character strings.
std::vector<uint8_t> txt_rdata(const std::vector<std::string>& values) {
    std::vector<uint8_t> result;
    for (const auto& value : values) {
        result.push_back(static_cast<uint8_t>(value.size()));
        result.insert(result.end(), value.begin(), value.end());
    }
    return result;
}

// Builds an mDNS response: 12-byte header (flags 0x8400 = response +
// authoritative, QDCOUNT=0, ANCOUNT/ARCOUNT set) followed by answer and
// additional records, each as name | type | class(IN, top bit = cache-flush)
// | TTL(32) | RDLENGTH | RDATA.
std::vector<uint8_t> make_packet(const std::vector<WireRecord>& answers,
                                 const std::vector<WireRecord>& additionals = {}) {
    std::vector<uint8_t> packet(12, 0);
    packet[2] = 0x84;
    packet[5] = 0;
    packet[6] = static_cast<uint8_t>(answers.size() >> 8U);
    packet[7] = static_cast<uint8_t>(answers.size() & 0xffU);
    packet[10] = static_cast<uint8_t>(additionals.size() >> 8U);
    packet[11] = static_cast<uint8_t>(additionals.size() & 0xffU);

    const auto append_record = [&packet](const WireRecord& record) {
        const auto owner = encode_name(record.owner);
        packet.insert(packet.end(), owner.begin(), owner.end());
        append_u16(packet, record.type);
        append_u16(packet, static_cast<uint16_t>(1U | (record.cache_flush ? 0x8000U : 0U)));
        append_u32(packet, record.ttl);
        append_u16(packet, static_cast<uint16_t>(record.data.size()));
        packet.insert(packet.end(), record.data.begin(), record.data.end());
    };
    for (const auto& record : answers) {
        append_record(record);
    }
    for (const auto& record : additionals) {
        append_record(record);
    }
    return packet;
}

// Convenience record builders for each supported record type.
WireRecord ptr(const std::string& owner, const std::string& target, uint32_t ttl = 120,
               bool cache_flush = false) {
    return {owner, static_cast<uint16_t>(RecordType::Ptr), ttl, cache_flush, name_rdata(target)};
}

WireRecord srv(const std::string& owner, const std::string& target, uint16_t port,
               uint32_t ttl = 120, bool cache_flush = true) {
    return {owner, static_cast<uint16_t>(RecordType::Srv), ttl, cache_flush,
            srv_rdata(port, target)};
}

WireRecord txt(const std::string& owner, const std::vector<std::string>& values,
               uint32_t ttl = 120, bool cache_flush = true) {
    return {owner, static_cast<uint16_t>(RecordType::Txt), ttl, cache_flush, txt_rdata(values)};
}

WireRecord a_record(const std::string& owner, std::array<uint8_t, 4> address,
                    uint32_t ttl = 120, bool cache_flush = true) {
    return {owner, static_cast<uint16_t>(RecordType::A), ttl, cache_flush,
            std::vector<uint8_t>(address.begin(), address.end())};
}

WireRecord aaaa_record(const std::string& owner, std::array<uint8_t, 16> address,
                       uint32_t ttl = 120, bool cache_flush = true) {
    return {owner, static_cast<uint16_t>(RecordType::Aaaa), ttl, cache_flush,
            std::vector<uint8_t>(address.begin(), address.end())};
}

// Minimal DNS-SD set for one IPP printer: type enumeration PTR, instance PTR,
// SRV and TXT.
std::vector<WireRecord> basic_ptr_records(uint32_t ttl = 120) {
    return {
        ptr("_services._dns-sd._udp.local", "_ipp._tcp.local", ttl),
        ptr("_ipp._tcp.local", "Office Printer._ipp._tcp.local", ttl),
        srv("Office Printer._ipp._tcp.local", "printer.local", 631, ttl),
        txt("Office Printer._ipp._tcp.local", {"ty=Laser", "note=ready"}, ttl),
    };
}

// Header with ANCOUNT=1 whose only record name is a bare compression pointer
// (0xC0 | offset) to `pointer`, used to test self/out-of-range pointers.
std::vector<uint8_t> make_pointer_failure_packet(uint16_t pointer) {
    std::vector<uint8_t> packet(12, 0);
    packet[6] = 0;
    packet[7] = 1;
    packet.push_back(static_cast<uint8_t>(0xc0U | ((pointer >> 8U) & 0x3fU)));
    packet.push_back(static_cast<uint8_t>(pointer & 0xffU));
    return packet;
}

// Two PTR answers using name compression: the second record's owner is a
// pointer to the first record's target, and its target is "Printer" followed
// by the same pointer. RDLENGTH fields are back-patched after writing.
std::vector<uint8_t> make_compressed_ptr_packet() {
    std::vector<uint8_t> packet(12, 0);
    packet[6] = 0;
    packet[7] = 2;

    const auto enumeration = encode_name("_services._dns-sd._udp.local");
    packet.insert(packet.end(), enumeration.begin(), enumeration.end());
    append_u16(packet, static_cast<uint16_t>(RecordType::Ptr));
    append_u16(packet, 1);
    append_u32(packet, 120);
    const auto first_length_offset = packet.size();
    append_u16(packet, 0);
    const auto service_type_offset = packet.size();
    const auto service_type = encode_name("_ipp._tcp.local");
    packet.insert(packet.end(), service_type.begin(), service_type.end());
    const auto first_length = static_cast<uint16_t>(packet.size() - service_type_offset);
    packet[first_length_offset] = static_cast<uint8_t>(first_length >> 8U);
    packet[first_length_offset + 1] = static_cast<uint8_t>(first_length & 0xffU);

    packet.push_back(static_cast<uint8_t>(0xc0U | ((service_type_offset >> 8U) & 0x3fU)));
    packet.push_back(static_cast<uint8_t>(service_type_offset & 0xffU));
    append_u16(packet, static_cast<uint16_t>(RecordType::Ptr));
    append_u16(packet, 1);
    append_u32(packet, 120);
    const auto second_length_offset = packet.size();
    append_u16(packet, 0);
    const auto instance_offset = packet.size();
    packet.push_back(7);
    packet.insert(packet.end(), {'P', 'r', 'i', 'n', 't', 'e', 'r'});
    packet.push_back(static_cast<uint8_t>(0xc0U | ((service_type_offset >> 8U) & 0x3fU)));
    packet.push_back(static_cast<uint8_t>(service_type_offset & 0xffU));
    const auto second_length = static_cast<uint16_t>(packet.size() - instance_offset);
    packet[second_length_offset] = static_cast<uint8_t>(second_length >> 8U);
    packet[second_length_offset + 1] = static_cast<uint8_t>(second_length & 0xffU);
    return packet;
}

// In-memory transport: records sent queries, replays queued packets, and
// advances a fake clock by the timeout when no packet is left.
class FakeBrowseTransport final : public platform::mdns::BrowseTransport {
public:
    using Query = std::pair<std::string, RecordType>;
    using Clock = platform::mdns::ServiceCache::Clock;

    explicit FakeBrowseTransport(std::vector<std::vector<uint8_t>> packets = {})
        : packets_(std::move(packets)) {}

    bool open(std::string&) override {
        opened = true;
        return true;
    }

    bool send_query(const std::string& name, RecordType type, std::string&) override {
        queries.emplace_back(name, type);
        return true;
    }

    platform::mdns::ReceiveStatus receive(std::chrono::milliseconds timeout,
                                          std::vector<uint8_t>& packet,
                                          std::string&) override {
        if (on_receive) on_receive();
        if (!packets_.empty()) {
            packet = std::move(packets_.front());
            packets_.erase(packets_.begin());
            return platform::mdns::ReceiveStatus::Packet;
        }
        current += timeout;
        return platform::mdns::ReceiveStatus::Timeout;
    }

    void close() override {
        closed = true;
    }

    Clock::time_point current{};
    std::vector<Query> queries;
    std::function<void()> on_receive;
    bool opened{false};
    bool closed{false};

private:
    std::vector<std::vector<uint8_t>> packets_;
};

} // namespace

int main() {
    bool passed = true;
    using Cache = platform::mdns::ServiceCache;
    const auto start = Cache::Clock::time_point{};

    // Parser: full record set and compression pointers.
    auto answers = basic_ptr_records();
    auto packet = make_packet(answers, {
        a_record("printer.local", {192, 0, 2, 44}),
        aaaa_record("printer.local", {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 1}),
    });
    const auto parsed = platform::mdns::parse_packet(packet);
    passed &= check(parsed.valid && !parsed.incomplete && parsed.records.size() == 6,
                    "parser reads PTR, SRV, TXT, A and AAAA across answer/additional sections");
    passed &= check(parsed.records.size() == 6 && parsed.records[0].target == "_ipp._tcp.local" &&
                        parsed.records[2].port == 631 && parsed.records[4].address == "192.0.2.44" &&
                        parsed.records[5].address == "2001:db8::1",
                    "parser extracts DNS-SD targets, service port, IPv4 and IPv6 values");

    const auto compressed = platform::mdns::parse_packet(make_compressed_ptr_packet());
    passed &= check(compressed.valid && compressed.records.size() == 2 &&
                        compressed.records[1].owner == "_ipp._tcp.local" &&
                        compressed.records[1].target == "Printer._ipp._tcp.local",
                    "parser expands valid compression pointers in owners and PTR targets");

    // Cache: deduplication, correlation, TTL, cache-flush and sanitisation.
    Cache cache;
    cache.ingest(parsed, start);
    cache.ingest(parsed, start + std::chrono::milliseconds(100));
    const auto types = cache.service_types(start + std::chrono::milliseconds(100));
    const auto services = cache.services(start + std::chrono::milliseconds(100));
    passed &= check(cache.record_count() == 6 && types == std::vector<std::string>{"_ipp._tcp"},
                    "cache deduplicates repeated records and enumerates discovered service types");
    passed &= check(services.size() == 1 && services[0].service_type == "_ipp._tcp" &&
                        services[0].instance == "Office Printer" &&
                        services[0].hostname == "printer.local" && services[0].port == 631 &&
                        services[0].addresses == std::vector<std::string>{"192.0.2.44", "2001:db8::1"} &&
                        services[0].text == std::vector<std::string>{"ty=Laser", "note=ready"},
                    "cache correlates PTR, SRV, TXT, A and AAAA records into a readable instance");

    Cache out_of_order;
    auto host_first = make_packet({
        srv("Office Printer._ipp._tcp.local", "printer.local", 631),
        txt("Office Printer._ipp._tcp.local", {"ty=Laser"}),
        a_record("printer.local", {192, 0, 2, 44}),
    });
    auto ptr_later = make_packet({
        ptr("_services._dns-sd._udp.local", "_ipp._tcp.local"),
        ptr("_ipp._tcp.local", "Office Printer._ipp._tcp.local"),
    });
    out_of_order.ingest(platform::mdns::parse_packet(host_first), start);
    out_of_order.ingest(platform::mdns::parse_packet(ptr_later), start + std::chrono::seconds(1));
    const auto reordered = out_of_order.services(start + std::chrono::seconds(1));
    passed &= check(reordered.size() == 1 && reordered[0].addresses == std::vector<std::string>{"192.0.2.44"},
                    "cache correlates records that arrive in separate, out-of-order packets");

    auto goodbye_records = basic_ptr_records();
    goodbye_records[1].ttl = 0;
    Cache goodbye_cache;
    goodbye_cache.ingest(parsed, start);
    goodbye_cache.ingest(platform::mdns::parse_packet(make_packet({goodbye_records[1]})),
                         start + std::chrono::seconds(1));
    passed &= check(goodbye_cache.services(start + std::chrono::seconds(1)).empty(),
                    "TTL-zero goodbye removes the matching service PTR immediately");

    auto short_lived = basic_ptr_records(1);
    Cache expiring_cache;
    expiring_cache.ingest(platform::mdns::parse_packet(make_packet(short_lived)), start);
    passed &= check(!expiring_cache.services(start + std::chrono::milliseconds(999)).empty() &&
                        expiring_cache.services(start + std::chrono::seconds(1)).empty(),
                    "cached records expire at their TTL boundary");

    Cache flush_cache;
    auto initial_srv = platform::mdns::parse_packet(make_packet({srv("Office Printer._ipp._tcp.local",
                                                                      "old-host.local", 631)}));
    auto replacement_srv = platform::mdns::parse_packet(make_packet({srv("Office Printer._ipp._tcp.local",
                                                                          "new-host.local", 631)}));
    flush_cache.ingest(initial_srv, start);
    flush_cache.ingest(replacement_srv, start + std::chrono::seconds(2));
    passed &= check(flush_cache.record_count() == 1,
                    "cache-flush removes stale unique records for the same owner and type");

    Cache incomplete_fields;
    incomplete_fields.ingest(platform::mdns::parse_packet(make_packet({
                                 ptr("_ipp._tcp.local", "Unresolved._ipp._tcp.local")})), start);
    const auto unresolved = incomplete_fields.services(start);
    passed &= check(unresolved.size() == 1 && unresolved[0].hostname.empty() &&
                        !unresolved[0].port && unresolved[0].addresses.empty(),
                    "service is retained with unresolved fields when SRV data is missing");

    auto unsafe_txt = txt("Print\ner._ipp._tcp.local", {std::string("key=value\nnext", 14)});
    const auto unsafe_packet = platform::mdns::parse_packet(make_packet({
        ptr("_ipp._tcp.local", "Print\ner._ipp._tcp.local"), unsafe_txt}));
    Cache safe_cache;
    safe_cache.ingest(unsafe_packet, start);
    const auto safe_services = safe_cache.services(start);
    passed &= check(unsafe_packet.valid && safe_services.size() == 1 &&
                        safe_services[0].instance.find('\n') == std::string::npos &&
                        safe_services[0].instance.find("\\x0A") != std::string::npos &&
                        safe_services[0].text.size() == 1 &&
                        safe_services[0].text[0].find('\n') == std::string::npos,
                    "control bytes in instance names and TXT metadata are escaped before UI display");

    // Parser robustness and resource budgets.
    auto truncated = packet;
    truncated.pop_back();
    passed &= check(!platform::mdns::parse_packet(truncated).valid,
                    "parser rejects truncated resource data");
    passed &= check(!platform::mdns::parse_packet(make_pointer_failure_packet(12)).valid &&
                        !platform::mdns::parse_packet(make_pointer_failure_packet(0x3fff)).valid,
                    "parser rejects cyclic/self and out-of-buffer compression pointers");

    std::vector<uint8_t> too_many(12, 0);
    too_many[6] = 0;
    too_many[7] = static_cast<uint8_t>(platform::mdns::kMaxRecordsPerPacket + 1);
    const auto capped_packet = platform::mdns::parse_packet(too_many);
    passed &= check(capped_packet.valid && capped_packet.incomplete && capped_packet.records.empty(),
                    "parser reports partial results instead of processing beyond the packet record budget");

    Cache capped_cache;
    for (std::size_t index = 0; index <= platform::mdns::kMaxCachedRecords; ++index) {
        const auto owner = "host" + std::to_string(index) + ".local";
        capped_cache.ingest(platform::mdns::parse_packet(make_packet({
                                a_record(owner, {192, 0, 2, static_cast<uint8_t>(index % 250 + 1)})})),
                            start);
    }
    passed &= check(capped_cache.record_count() == platform::mdns::kMaxCachedRecords &&
                        capped_cache.cache_bytes() <= platform::mdns::kMaxCacheBytes &&
                        capped_cache.incomplete(),
                    "cache memory/record budgets are enforced and mark the result incomplete");

    // Query encoding.
    const auto query_packet = platform::mdns::make_query_packet("Printer._ipp._tcp.local", RecordType::Ptr);
    passed &= check(!query_packet.empty() && query_packet.size() > 12 &&
                        query_packet[5] == 1 && platform::mdns::parse_packet(query_packet).valid,
                    "DNS-SD questions are encoded as valid bounded DNS packets");
    passed &= check(!platform::mdns::make_query_packet(std::string(64, 'x') + ".local", RecordType::Ptr).size(),
                    "query builder rejects DNS labels exceeding the wire limit");

    // End-to-end browse over the fake transport: success, empty, cancel, budget.
    auto enumeration = make_packet({
        ptr("_services._dns-sd._udp.local", "_ipp._tcp.local"),
        ptr("_ipp._tcp.local", "Office Printer._ipp._tcp.local"),
    });
    auto service_metadata = make_packet({
        srv("Office Printer._ipp._tcp.local", "printer.local", 631),
        txt("Office Printer._ipp._tcp.local", {"ty=Laser"}),
    });
    auto service_addresses = make_packet({
        a_record("printer.local", {192, 0, 2, 44}),
        aaaa_record("printer.local", {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                       0, 0, 0, 0, 0, 0, 0, 1}),
    });
    FakeBrowseTransport browse_transport({enumeration, service_metadata, service_addresses});
    std::atomic<bool> browse_cancel{false};
    const auto browse_result = platform::mdns::browse_mdns(
        browse_cancel,
        browse_transport,
        [&browse_transport] { return browse_transport.current; },
        std::chrono::milliseconds(120));
    const auto contains_query = [&browse_transport](const std::string& name, RecordType type) {
        return std::find(browse_transport.queries.begin(), browse_transport.queries.end(),
                         FakeBrowseTransport::Query{name, type}) != browse_transport.queries.end();
    };
    passed &= check(browse_result.status == model::EthernetTestStatus::Passed &&
                        browse_transport.opened && browse_transport.closed,
                    "browse uses and closes the injected transport without opening a real socket");
    passed &= check(contains_query("_services._dns-sd._udp.local", RecordType::Ptr) &&
                        contains_query("_ipp._tcp.local", RecordType::Ptr) &&
                        contains_query("Office Printer._ipp._tcp.local", RecordType::Srv) &&
                        contains_query("Office Printer._ipp._tcp.local", RecordType::Txt) &&
                        contains_query("printer.local", RecordType::A) &&
                        contains_query("printer.local", RecordType::Aaaa),
                    "browse progresses from service enumeration to instance and host queries");
    passed &= check(browse_result.detail.find("mDNS services found (1)") != std::string::npos &&
                        browse_result.detail.find("Port: 631") != std::string::npos &&
                        browse_result.detail.find("Address: 192.0.2.44") != std::string::npos &&
                        browse_result.detail.find("Address: 2001:db8::1") != std::string::npos &&
                        browse_result.detail.find("TXT: ty=Laser") != std::string::npos,
                    "browse result renders resolved DNS-SD fields including IPv6 and TXT");

    FakeBrowseTransport quiet_transport;
    std::atomic<bool> quiet_cancel{false};
    const auto quiet_result = platform::mdns::browse_mdns(
        quiet_cancel,
        quiet_transport,
        [&quiet_transport] { return quiet_transport.current; },
        std::chrono::milliseconds(100));
    passed &= check(quiet_result.status == model::EthernetTestStatus::Passed &&
                        quiet_transport.queries.size() == 1 && quiet_transport.current ==
                            Cache::Clock::time_point{} + std::chrono::milliseconds(100) &&
                        quiet_result.detail.find("does not indicate network failure") != std::string::npos,
                    "empty discovery window is a completed browse, not a network-failure claim");

    FakeBrowseTransport cancel_transport;
    std::atomic<bool> cancel_during_browse{false};
    cancel_transport.on_receive = [&cancel_during_browse] { cancel_during_browse.store(true); };
    const auto cancel_result = platform::mdns::browse_mdns(
        cancel_during_browse,
        cancel_transport,
        [&cancel_transport] { return cancel_transport.current; },
        std::chrono::seconds(1));
    passed &= check(cancel_result.status == model::EthernetTestStatus::Cancelled &&
                        cancel_transport.closed && cancel_transport.queries.size() == 1,
                    "browse cancellation interrupts the receive loop and closes its transport");

    std::vector<WireRecord> many_service_types;
    for (std::size_t index = 0; index < platform::mdns::kMaxBrowseQueries + 4; ++index) {
        many_service_types.push_back(ptr("_services._dns-sd._udp.local",
                                         "_type" + std::to_string(index) + "._tcp.local"));
    }
    FakeBrowseTransport budget_transport({make_packet(many_service_types)});
    std::atomic<bool> budget_cancel{false};
    const auto budget_result = platform::mdns::browse_mdns(
        budget_cancel,
        budget_transport,
        [&budget_transport] { return budget_transport.current; },
        std::chrono::milliseconds(100));
    passed &= check(budget_transport.queries.size() == platform::mdns::kMaxBrowseQueries &&
                        budget_result.status == model::EthernetTestStatus::Failed &&
                        budget_result.detail.find("INCOMPLETE") != std::string::npos,
                    "query budget stops growth and marks the browse incomplete");

    if (!passed) {
        return 1;
    }
    std::cout << "Ethernet mDNS parser and cache tests passed.\n";
    return 0;
}

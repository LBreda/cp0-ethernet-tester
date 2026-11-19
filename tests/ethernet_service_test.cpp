/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ethernet_service.h"
#include "ethernet_diagnostics.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Prints a failure message when the condition is false; returns the condition.
bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

// Polls the service until it reports `expected` or the timeout elapses.
bool wait_for_status(platform::EthernetService& service,
                     model::EthernetTestStatus expected,
                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (service.result().status == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return service.result().status == expected;
}

// Polls until the service leaves the Ready/Running states or times out.
bool wait_for_completion(platform::EthernetService& service,
                         std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = service.result().status;
        if (status != model::EthernetTestStatus::Running &&
            status != model::EthernetTestStatus::Ready) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto status = service.result().status;
    return status != model::EthernetTestStatus::Running &&
           status != model::EthernetTestStatus::Ready;
}

// LLDP TLV: 7-bit type and 9-bit length packed in a 16-bit header, then value.
void append_lldp_tlv(std::vector<uint8_t>& frame, uint8_t type, const std::vector<uint8_t>& value) {
    const auto header = static_cast<uint16_t>((static_cast<uint16_t>(type) << 9U) | value.size());
    frame.push_back(static_cast<uint8_t>(header >> 8U));
    frame.push_back(static_cast<uint8_t>(header & 0xffU));
    frame.insert(frame.end(), value.begin(), value.end());
}

// Ethernet II LLDP frame (zeroed MACs, EtherType 0x88CC) with TLVs:
// chassis ID (subtype 4) "sw1", port ID (subtype 5) "Gi1", port description,
// system name, IEEE 802.1 port VLAN ID 42 (OUI 00-80-C2), End of LLDPDU.
std::vector<uint8_t> make_lldp_frame() {
    std::vector<uint8_t> frame(12, 0);
    frame.insert(frame.end(), {0x88, 0xcc});
    append_lldp_tlv(frame, 1, {4, 's', 'w', '1'});
    append_lldp_tlv(frame, 2, {5, 'G', 'i', '1'});
    append_lldp_tlv(frame, 4, {'u', 'p', 'l', 'i', 'n', 'k'});
    append_lldp_tlv(frame, 5, {'s', 'w', 'i', 't', 'c', 'h'});
    append_lldp_tlv(frame, 127, {0x00, 0x80, 0xc2, 1, 0, 42});
    append_lldp_tlv(frame, 0, {});
    return frame;
}

// CDP TLV: type(16) | length(16, includes the 4-byte header) | value.
void append_cdp_tlv(std::vector<uint8_t>& payload, uint16_t type, const std::vector<uint8_t>& value) {
    const auto length = static_cast<uint16_t>(value.size() + 4);
    payload.push_back(static_cast<uint8_t>(type >> 8U));
    payload.push_back(static_cast<uint8_t>(type & 0xffU));
    payload.push_back(static_cast<uint8_t>(length >> 8U));
    payload.push_back(static_cast<uint8_t>(length & 0xffU));
    payload.insert(payload.end(), value.begin(), value.end());
}

// 802.3 frame (zeroed MACs, length field) with LLC/SNAP header (OUI 00-00-0C,
// PID 0x2000), CDP v2 header (TTL 180, checksum 0) and TLVs: device ID,
// port ID and native VLAN 42.
std::vector<uint8_t> make_cdp_frame() {
    std::vector<uint8_t> payload = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x0c, 0x20, 0x00,
                                    0x02, 0xb4, 0x00, 0x00};
    append_cdp_tlv(payload, 0x0001, {'c', 'd', 'p', '-','s', 'w'});
    append_cdp_tlv(payload, 0x0003, {'G', 'i', '1', '/', '0', '/', '1'});
    append_cdp_tlv(payload, 0x000a, {0, 42});
    std::vector<uint8_t> frame(12, 0);
    frame.push_back(static_cast<uint8_t>(payload.size() >> 8U));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xffU));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// Turns a PTR query into a response (flags 0x8180, ANCOUNT=1) and appends an
// answer whose name is a pointer to the question (0xC00C), type PTR, class IN,
// TTL 300, RDATA "host.example".
std::vector<uint8_t> make_ptr_response(uint16_t transaction_id) {
    auto packet = platform::diagnostics::make_ptr_query("192.0.2.1", transaction_id);
    packet[2] = 0x81;
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = 1;
    packet.insert(packet.end(), {0xc0, 0x0c, 0x00, 0x0c, 0x00, 0x01,
                                 0x00, 0x00, 0x01, 0x2c, 0x00, 0x0e,
                                 0x04, 'h', 'o', 's', 't',
                                 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x00});
    return packet;
}

} // namespace

int main() {
    bool passed = true;
    // Input validation and command-argument construction.
    passed &= check(platform::EthernetService::is_valid_ipv4_address("192.168.10.1"),
                    "valid IPv4 gateway is accepted");
    passed &= check(!platform::EthernetService::is_valid_ipv4_address("192.168.1.1;id"),
                    "shell metacharacters are rejected");
    passed &= check(!platform::EthernetService::is_valid_ipv4_address("2001:db8::1"),
                    "IPv6 is rejected by the IPv4 gateway probe");

    const auto arguments = platform::EthernetService::gateway_ping_arguments("192.168.10.1");
    passed &= check(arguments == std::vector<std::string>{
                        "-n", "-c", "1", "-W", "2", "-I", "eth0", "192.168.10.1"},
                    "gateway probe uses exactly one bounded packet on eth0");
    passed &= check(platform::EthernetService::gateway_ping_arguments("192.168.1.1 && id").empty(),
                    "invalid destination produces no command arguments");

    // Performance snapshot reads local counters only.
    platform::EthernetService service;
    passed &= check(service.start(model::EthernetCategory::Performance),
                    "performance snapshot starts without network traffic");
    passed &= check(wait_for_completion(service, std::chrono::milliseconds(1000)),
                    "performance snapshot completes or reports unavailable counters");
    const auto performance = service.result();
    passed &= check(performance.summary == "Interface counters read" ||
                        performance.summary == "Interface counters unavailable" ||
                        performance.summary == "eth0 not found",
                    "performance result describes local interface counters");
    passed &= check(performance.detail.find("requires explicit support") == std::string::npos,
                    "performance does not return the generic unsupported result");
    passed &= check(performance.summary == "Interface counters read"
                        ? performance.status == model::EthernetTestStatus::Passed
                        : performance.status == model::EthernetTestStatus::Unsupported,
                    "performance status matches counter availability");

    // arp-scan discovery arguments and output parsing.
    const auto scan_arguments = platform::EthernetService::discovery_scan_arguments();
    passed &= check(scan_arguments == std::vector<std::string>{
                        "--interface=eth0", "--localnet", "--retry=1", "--timeout=500",
                        "--bandwidth=100000"},
                    "discovery arguments target only eth0's local subnet with bounded retries and rate");

    const std::string scan_output =
        "Interface: eth0, type: EN10MB, MAC: 00:11:22:33:44:55, IPv4: 192.168.10.2\n"
        "Starting arp-scan 1.10.0 with 256 hosts\n"
        "192.168.10.1  00:11:22:33:44:01  Router\n"
        "192.168.10.2  00:11:22:33:44:02  Device\n"
        "192.168.10.3  00:11:22:33:44:03  Device\n"
        "192.168.10.4  00:11:22:33:44:04  Device\n"
        "192.168.10.5  00:11:22:33:44:05  Device\n"
        "192.168.10.6  00:11:22:33:44:06  Device\n"
        "192.168.10.7  00:11:22:33:44:07  Device\n"
        "192.168.10.7  00:11:22:33:44:07  Duplicate\n"
        "packets received by filter\n";
    const auto peers = platform::EthernetService::parse_discovery_scan_output(scan_output);
    passed &= check(peers.size() == 7,
                    "discovery parser retains every responding peer beyond the old five-entry limit");
    passed &= check(peers.size() == 7 && peers.front().first == "192.168.10.1" &&
                        peers.back().second == "00:11:22:33:44:07",
                    "discovery parser extracts IPv4/MAC rows and ignores headers and duplicates");
    passed &= check(platform::EthernetService::parse_discovery_scan_output(
                        "192.168.10.8  invalid-mac  Device\n999.1.1.1  00:11:22:33:44:08  Bad\n").empty(),
                    "discovery parser rejects malformed addresses and MACs");

    // Neighbor announcement parsers (LLDP/CDP).
    const auto lldp = make_lldp_frame();
    const auto parsed_lldp = platform::diagnostics::parse_lldp_frame(lldp);
    passed &= check(parsed_lldp && parsed_lldp->protocol == "LLDP" &&
                        parsed_lldp->chassis_id == "sw1" && parsed_lldp->port_id == "Gi1" &&
                        parsed_lldp->system_name == "switch" &&
                        parsed_lldp->port_description == "uplink" && parsed_lldp->vlan == "42",
                    "LLDP parser extracts only bounded announced identity, port, name, and VLAN fields");
    auto truncated_lldp = lldp;
    truncated_lldp.pop_back();
    passed &= check(!platform::diagnostics::parse_lldp_frame(truncated_lldp),
                    "LLDP parser rejects a truncated end TLV");

    const auto cdp = make_cdp_frame();
    const auto parsed_cdp = platform::diagnostics::parse_cdp_frame(cdp);
    passed &= check(parsed_cdp && parsed_cdp->protocol == "CDP" &&
                        parsed_cdp->chassis_id == "cdp-sw" &&
                        parsed_cdp->port_id == "Gi1/0/1" && parsed_cdp->vlan == "42",
                    "CDP parser extracts device, port, and native VLAN from a synthetic frame");
    auto truncated_cdp = cdp;
    truncated_cdp.pop_back();
    passed &= check(!platform::diagnostics::parse_cdp_frame(truncated_cdp),
                    "CDP parser rejects a frame shorter than its declared 802.3 payload");
    passed &= check(platform::diagnostics::parse_neighbor_frame(lldp)->protocol == "LLDP" &&
                        platform::diagnostics::parse_neighbor_frame(cdp)->protocol == "CDP",
                    "neighbor parser identifies both supported announcement protocols");

    // Gateway reverse-DNS query/response and counter deltas.
    const auto ptr_name = platform::diagnostics::gateway_ptr_query_name("192.0.2.1");
    passed &= check(ptr_name && *ptr_name == "1.2.0.192.in-addr.arpa",
                    "gateway address is reversed into its in-addr.arpa PTR name");
    passed &= check(!platform::diagnostics::gateway_ptr_query_name("192.0.2.1;id") &&
                        platform::diagnostics::make_ptr_query("192.0.2.1;id", 0x1234).empty(),
                    "invalid gateway input cannot form a DNS query");
    const uint16_t dns_id = 0x6a31;
    const auto query = platform::diagnostics::make_ptr_query("192.0.2.1", dns_id);
    passed &= check(query.size() > 12 && query[0] == 0x6a && query[1] == 0x31 &&
                        query[2] == 0x01 && query[5] == 0x01 &&
                        query[query.size() - 4] == 0x00 && query[query.size() - 3] == 0x0c &&
                        query[query.size() - 2] == 0x00 && query.back() == 0x01,
                    "DNS query is one recursive IN PTR question with a caller-provided transaction ID");
    const auto ptr_packet = make_ptr_response(dns_id);
    const auto ptr_response = platform::diagnostics::parse_ptr_response(
        ptr_packet, dns_id, "1.2.0.192.in-addr.arpa");
    passed &= check(ptr_response.valid && !ptr_response.truncated && ptr_response.response_code == 0 &&
                        ptr_response.ptr_records == std::vector<std::string>{"host.example"},
                    "DNS parser reads a compressed PTR answer for the expected gateway question");
    auto truncated_ptr = ptr_packet;
    truncated_ptr.pop_back();
    passed &= check(!platform::diagnostics::parse_ptr_response(
                        truncated_ptr, dns_id, "1.2.0.192.in-addr.arpa").valid,
                    "DNS parser rejects truncated record data");
    passed &= check(!platform::diagnostics::parse_ptr_response(
                        ptr_packet, static_cast<uint16_t>(dns_id + 1), "1.2.0.192.in-addr.arpa").valid &&
                        !platform::diagnostics::parse_ptr_response(
                            ptr_packet, dns_id, "2.2.0.192.in-addr.arpa").valid,
                    "DNS parser rejects mismatched transaction IDs and question names");
    passed &= check(platform::diagnostics::counter_delta(100, 145).value == 45 &&
                        !platform::diagnostics::counter_delta(100, 145).reset &&
                        platform::diagnostics::counter_delta(145, 3).value == 3 &&
                        platform::diagnostics::counter_delta(145, 3).reset,
                    "counter deltas distinguish increments from counter resets");

    // Cancellation via an injected runner that spins until cancelled.
    std::atomic<bool> runner_entered{false};
    platform::EthernetService cancellable([&runner_entered](model::EthernetCategory,
                                                            const std::atomic<bool>& cancel_requested) {
        runner_entered.store(true);
        while (!cancel_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return model::EthernetTestResult{model::EthernetTestStatus::Cancelled,
                                         "Test cancelled",
                                         "Test runner observed cancellation."};
    });
    passed &= check(cancellable.start(model::EthernetCategory::Link),
                    "test runner starts for cancellation check");
    const auto runner_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!runner_entered.load() && std::chrono::steady_clock::now() < runner_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    passed &= check(runner_entered.load(), "cancellable runner was entered");
    cancellable.cancel();
    passed &= check(wait_for_status(cancellable, model::EthernetTestStatus::Cancelled,
                                   std::chrono::seconds(1)),
                    "running service propagates cancellation without network access");

    if (!passed) {
        return 1;
    }
    std::cout << "Ethernet service safety tests passed.\n";
    return 0;
}
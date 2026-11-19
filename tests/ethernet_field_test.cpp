/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ethernet_field.h"
#include "ethernet_model.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    // Prints a failure message when the condition is false; returns the condition.
    bool check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            return false;
        }
        return true;
    }

    // Big-endian helpers: append, or overwrite at a fixed offset.
    void append_u16(std::vector<uint8_t>& bytes, uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value >> 8U));
        bytes.push_back(static_cast<uint8_t>(value & 0xffU));
    }

    void put_u16(std::vector<uint8_t>& bytes, std::size_t offset, uint16_t value)
    {
        bytes[offset] = static_cast<uint8_t>(value >> 8U);
        bytes[offset + 1] = static_cast<uint8_t>(value & 0xffU);
    }

    void append_ipv4(std::vector<uint8_t>& bytes, const std::array<uint8_t, 4>& address)
    {
        bytes.insert(bytes.end(), address.begin(), address.end());
    }

    // Ethernet II ARP reply: dst MAC (zeros) | src MAC | EtherType 0x0806 |
    // HTYPE=1 | PTYPE=0x0800 | HLEN=6 | PLEN=4 | OPER=2 (reply) |
    // sender MAC/IP | target MAC (zeros) | target IP 192.0.2.1.
    std::vector<uint8_t> make_arp_frame(const std::array<uint8_t, 6>& mac,
                                        const std::array<uint8_t, 4>& sender_ip)
    {
        std::vector<uint8_t> frame(6, 0);
        frame.insert(frame.end(), mac.begin(), mac.end());
        append_u16(frame, 0x0806);
        append_u16(frame, 1);
        append_u16(frame, 0x0800);
        frame.push_back(6);
        frame.push_back(4);
        append_u16(frame, 2);
        frame.insert(frame.end(), mac.begin(), mac.end());
        append_ipv4(frame, sender_ip);
        frame.insert(frame.end(), 6, 0);
        append_ipv4(frame, {192, 0, 2, 1});
        return frame;
    }

    // Ethernet II / IPv4 (IHL=5, TTL=64, proto=UDP) / UDP 67->68 / BOOTP reply
    // (op=2, htype=1, hlen=6, yiaddr=offered, magic cookie 63 82 53 63)
    // followed by options 53=OFFER, 54=server identifier, 255=end.
    // IP and UDP checksums are left zero.
    std::vector<uint8_t> make_dhcp_offer(const std::array<uint8_t, 4>& server,
                                         const std::array<uint8_t, 4>& offered)
    {
        std::vector<uint8_t> options = {53, 1, 2, 54, 4};
        append_ipv4(options, server);
        options.push_back(255);
        constexpr std::size_t kBootpLength = 240;
        const auto udp_length = static_cast<uint16_t>(8 + kBootpLength + options.size());
        const auto ip_length = static_cast<uint16_t>(20 + udp_length);

        std::vector<uint8_t> frame(6, 0);
        frame.insert(frame.end(), {0x02, 0, 0, 0, 0, 1});
        append_u16(frame, 0x0800);
        frame.resize(frame.size() + 20, 0);
        const auto ip_offset = std::size_t{14};
        frame[ip_offset] = 0x45;
        put_u16(frame, ip_offset + 2, ip_length);
        frame[ip_offset + 8] = 64;
        frame[ip_offset + 9] = 17;
        std::copy(server.begin(), server.end(), frame.begin() + static_cast<std::ptrdiff_t>(ip_offset + 12));
        const std::array<uint8_t, 4> broadcast{255, 255, 255, 255};
        std::copy(broadcast.begin(), broadcast.end(),
                  frame.begin() + static_cast<std::ptrdiff_t>(ip_offset + 16));

        append_u16(frame, 67);
        append_u16(frame, 68);
        append_u16(frame, udp_length);
        append_u16(frame, 0);
        const auto bootp_offset = frame.size();
        frame.resize(frame.size() + kBootpLength, 0);
        frame[bootp_offset] = 2;
        frame[bootp_offset + 1] = 1;
        frame[bootp_offset + 2] = 6;
        std::copy(offered.begin(), offered.end(),
                  frame.begin() + static_cast<std::ptrdiff_t>(bootp_offset + 16));
        frame[bootp_offset + 236] = 0x63;
        frame[bootp_offset + 237] = 0x82;
        frame[bootp_offset + 238] = 0x53;
        frame[bootp_offset + 239] = 0x63;
        frame.insert(frame.end(), options.begin(), options.end());
        return frame;
    }

    // 802.1Q-tagged 802.3 frame carrying an STP BPDU: dst MAC (0x01..) | src MAC |
    // TPID 0x8100 | TCI (VLAN 1) | length 7 | LLC 42 42 03 |
    // protocol id 0 | BPDU version | BPDU type.
    std::vector<uint8_t> make_vlan_stp_frame(const std::array<uint8_t, 6>& source_mac,
                                             uint8_t version,
                                             uint8_t type)
    {
        std::vector<uint8_t> frame(6, 0x01);
        frame.insert(frame.end(), source_mac.begin(), source_mac.end());
        append_u16(frame, 0x8100);
        append_u16(frame, 1);
        append_u16(frame, 7);
        frame.insert(frame.end(), {0x42, 0x42, 0x03, 0, 0, version, type});
        return frame;
    }
} // namespace

int main()
{
    bool passed = true;

    // DHCP lease parsers (systemd-networkd and NetworkManager formats).
    const auto lease = platform::field::parse_networkd_lease(
        "ADDRESS=192.0.2.10/24\nROUTER=192.0.2.1\nSERVER_ADDRESS=192.0.2.254\n"
        "DNS=192.0.2.53 192.0.2.54 192.0.2.53\nLIFETIME=3600\nT1=1800\nT2=3150\n");
    passed &= check(lease && lease->address == "192.0.2.10" && lease->router == "192.0.2.1" &&
                    lease->server_address == "192.0.2.254" && lease->dns_servers.size() == 2 &&
                    lease->lifetime_seconds == 3600 && lease->renewal_seconds == 1800 &&
                    lease->rebinding_seconds == 3150,
                    "lease parser reads IPv4 address, gateway, server, DNS and lease timers");
    passed &= check(!platform::field::parse_networkd_lease("ROUTER=192.0.2.1\n") &&
                    !platform::field::parse_networkd_lease("ADDRESS=192.0.2.999\n") &&
                    !platform::field::parse_networkd_lease(
                        std::string(platform::field::kMaxLeaseBytes + 1, 'x')),
                    "lease parser rejects missing address, invalid IPv4 and oversized input");

    const auto networkmanager_lease = platform::field::parse_networkmanager_lease(
        "GENERAL.DEVICE:eth0\nIP4.ADDRESS[1]:192.168.0.161/24\n"
        "DHCP4.OPTION[1]:dhcp_lease_time = 864000\n"
        "DHCP4.OPTION[2]:dhcp_server_identifier = 192.168.0.1\n"
        "DHCP4.OPTION[3]:ip_address = 192.168.0.161\n"
        "DHCP4.OPTION[4]:routers = 192.168.0.1\n"
        "DHCP4.OPTION[5]:domain_name_servers = 192.168.0.1\n",
        "eth0", "192.168.0.161");
    passed &= check(networkmanager_lease && networkmanager_lease->address == "192.168.0.161" &&
                    networkmanager_lease->router == "192.168.0.1" &&
                    networkmanager_lease->server_address == "192.168.0.1" &&
                    networkmanager_lease->dns_servers == std::vector<std::string>{"192.168.0.1"} &&
                    networkmanager_lease->lifetime_seconds == 864000,
                    "NetworkManager parser reads matching current DHCP lease fields");
    passed &= check(!platform::field::parse_networkmanager_lease(
                        "GENERAL.DEVICE:eth1\nIP4.ADDRESS[1]:192.168.0.161/24\n"
                        "DHCP4.OPTION[1]:ip_address = 192.168.0.161\n",
                        "eth0", "192.168.0.161") &&
                    !platform::field::parse_networkmanager_lease(
                        "GENERAL.DEVICE:eth0\nIP4.ADDRESS[1]:192.168.0.161/24\n"
                        "DHCP4.OPTION[1]:ip_address = 192.168.0.160\n",
                        "eth0", "192.168.0.161") &&
                    !platform::field::parse_networkmanager_lease(
                        "GENERAL.DEVICE:eth0\nIP4.ADDRESS[1]:192.168.0.160/24\n"
                        "DHCP4.OPTION[1]:ip_address = 192.168.0.161\n",
                        "eth0", "192.168.0.161") &&
                    !platform::field::parse_networkmanager_lease(
                        "GENERAL.DEVICE:eth0\nIP4.ADDRESS[1]:192.168.0.161/24\n"
                        "DHCP4.OPTION[1]:ip_address = 192.168.0.999\n",
                        "eth0", "192.168.0.161") &&
                    !platform::field::parse_networkmanager_lease(
                        "GENERAL.DEVICE:eth0\nIP4.ADDRESS[1]:192.168.0.161/24\n",
                        "eth0", "192.168.0.161") &&
                    !platform::field::parse_networkmanager_lease(
                        std::string(platform::field::kMaxLeaseBytes + 1, 'x'),
                        "eth0", "192.168.0.161"),
                    "NetworkManager parser rejects wrong device, stale/missing DHCP data and oversized output");

    // Raw frame parsing: ARP, DHCPOFFER and STP.
    const std::array<uint8_t, 6> mac_a{0x02, 0, 0, 0, 0, 1};
    const std::array<uint8_t, 6> mac_b{0x02, 0, 0, 0, 0, 2};
    const auto arp_a = platform::field::parse_ethernet_frame(
        make_arp_frame(mac_a, {192, 0, 2, 10}));
    const auto arp_b = platform::field::parse_ethernet_frame(
        make_arp_frame(mac_b, {192, 0, 2, 10}));
    passed &= check(arp_a && arp_a->protocol == platform::field::Protocol::Arp &&
                    arp_a->ipv4 == "192.0.2.10" && arp_a->related_ipv4 == "192.0.2.1" &&
                    arp_a->source_mac == "02:00:00:00:00:01",
                    "ARP parser extracts sender and target from Ethernet/IPv4 ARP frames");
    passed &= check(!platform::field::parse_ethernet_frame({0, 1, 2}) &&
                    !platform::field::parse_ethernet_frame(std::vector<uint8_t>(30, 0)),
                    "frame parser rejects truncated and unsupported Ethernet data");

    const auto offer_a = platform::field::parse_ethernet_frame(
        make_dhcp_offer({192, 0, 2, 254}, {192, 0, 2, 10}));
    const auto offer_b = platform::field::parse_ethernet_frame(
        make_dhcp_offer({192, 0, 2, 253}, {192, 0, 2, 11}));
    passed &= check(offer_a && offer_a->protocol == platform::field::Protocol::DhcpOffer &&
                    offer_a->server_ipv4 == "192.0.2.254" &&
                    offer_a->offered_ipv4 == "192.0.2.10",
                    "DHCP parser accepts bounded DHCPOFFER packets and extracts server/address");
    auto malformed_offer = make_dhcp_offer({192, 0, 2, 254}, {192, 0, 2, 10});
    malformed_offer.pop_back();
    passed &= check(!platform::field::parse_ethernet_frame(malformed_offer),
                    "DHCP parser rejects truncated options");

    const auto stp = platform::field::parse_ethernet_frame(make_vlan_stp_frame(mac_a, 2, 2));
    passed &= check(stp && stp->protocol == platform::field::Protocol::Stp &&
                    stp->bpdu_version == 2 && stp->bpdu_type == 2,
                    "STP parser handles tagged 802.3 LLC BPDU frames");

    // Observation aggregation and record budget.
    platform::field::ObservationAccumulator observations;
    observations.ingest(*arp_a);
    observations.ingest(*arp_a);
    observations.ingest(*arp_b);
    observations.ingest(*offer_a);
    observations.ingest(*offer_b);
    observations.ingest(*stp);
    const auto snapshot = observations.summary();
    passed &= check(snapshot.arp_frames == 3 && snapshot.arp_senders.size() == 1 &&
                    snapshot.arp_senders.front().mac_addresses.size() == 2 &&
                    snapshot.possible_ip_conflicts.size() == 1,
                    "observer deduplicates ARP MACs and flags only possible IP conflicts");
    passed &= check(snapshot.dhcp_offer_frames == 2 && snapshot.dhcp_servers.size() == 2 &&
                    snapshot.stp_frames == 1,
                    "observer counts offers and distinct DHCP servers without labeling them rogue");
    for (std::size_t index = 0; index < platform::field::kMaxObservedFrames + 1; ++index)
    {
        observations.ingest(*stp);
    }
    passed &= check(observations.summary().incomplete,
                    "observer signals when its finite record budget is exceeded");

    // EthernetModel: per-category results and health summary / staleness.
    model::EthernetModel model;
    model.set_result(model::EthernetCategory::Link,
                     {model::EthernetTestStatus::Passed, "Link OK", "Carrier is up."});
    const auto link_result = model.result(model::EthernetCategory::Link);
    model.set_category(model::EthernetCategory::Dns);
    model.set_result(model::EthernetCategory::Dns,
                     {model::EthernetTestStatus::Failed, "DNS timeout", "Resolver did not reply."});
    model.set_category(model::EthernetCategory::Link);
    passed &= check(model.result().summary == "Link OK" && link_result.has_completed_at &&
                    model.result().has_completed_at && model.result().completed_at.time_since_epoch().count() > 0 &&
                    model.result(model::EthernetCategory::Dns).summary == "DNS timeout",
                    "model preserves timestamped diagnostic results independently by category");
    const auto health = model.health_summary();
    passed &= check(health.blocks[0].state == model::EthernetHealthState::Green &&
                    health.blocks[0].has_measurement &&
                    health.blocks[1].state == model::EthernetHealthState::Red &&
                    health.blocks[2].state == model::EthernetHealthState::NotTested,
                    "summary combines the worst completed state while retaining untested blocks");
    auto latest_link_detail = model.result(model::EthernetCategory::Advanced);
    latest_link_detail.status = model::EthernetTestStatus::Passed;
    latest_link_detail.summary = "Advanced diagnostics sampled";
    latest_link_detail.detail = "Driver metadata is available.";
    latest_link_detail.completed_at = link_result.completed_at + std::chrono::seconds(3);
    latest_link_detail.has_completed_at = true;
    model.set_result(model::EthernetCategory::Advanced, latest_link_detail);
    const auto refreshed_health = model.health_summary();
    passed &= check(refreshed_health.blocks[0].measured_at == latest_link_detail.completed_at,
                    "summary age is based on the most recent measurement in each block");

    {
        model::EthernetModel changed_carrier;
        auto changed_link = model::EthernetTestResult{
            model::EthernetTestStatus::Passed,
            "Link sampled",
            "Observed carrier changes: 1"
        };
        changed_link.completed_at = std::chrono::system_clock::now();
        changed_link.has_completed_at = true;
        auto previous_configuration = model::EthernetTestResult{
            model::EthernetTestStatus::Passed,
            "Configuration captured",
            "IPv4 address available."
        };
        previous_configuration.completed_at = changed_link.completed_at - std::chrono::seconds(2);
        previous_configuration.has_completed_at = true;
        changed_carrier.set_result(model::EthernetCategory::Link, changed_link);
        changed_carrier.set_result(model::EthernetCategory::Configuration, previous_configuration);
        const auto stale_health = changed_carrier.health_summary();
        passed &= check(stale_health.blocks[1].state == model::EthernetHealthState::Yellow &&
                        stale_health.blocks[1].stale,
                        "observed carrier changes mark older IP/DNS measurements stale");
        auto post_change_dns = model::EthernetTestResult{
            model::EthernetTestStatus::Passed,
            "DNS checked",
            "Gateway lookup completed."
        };
        post_change_dns.completed_at = changed_link.completed_at + std::chrono::seconds(1);
        post_change_dns.has_completed_at = true;
        changed_carrier.set_result(model::EthernetCategory::Dns, post_change_dns);
        const auto post_change_health = changed_carrier.health_summary();
        passed &= check(!post_change_health.blocks[1].stale &&
                        post_change_health.blocks[1].state == model::EthernetHealthState::Green,
                        "measurements made after the observed carrier change remain current");
    }

    model::EthernetModel changed_link;
    changed_link.set_result(model::EthernetCategory::Configuration,
                            {model::EthernetTestStatus::Passed, "IP configured", "Address found."});
    changed_link.set_result(model::EthernetCategory::Neighbors,
                            {model::EthernetTestStatus::Passed, "Listen complete", "No announcements."});
    changed_link.set_result(model::EthernetCategory::Link,
                            {
                                model::EthernetTestStatus::Passed,
                                "Link sample complete",
                                "Observed carrier changes: 1"
                            });
    const auto stale_health = changed_link.health_summary();
    passed &= check(stale_health.blocks[0].state == model::EthernetHealthState::Green &&
                    stale_health.blocks[1].state == model::EthernetHealthState::Yellow &&
                    stale_health.blocks[1].stale &&
                    stale_health.blocks[2].state == model::EthernetHealthState::Yellow &&
                    stale_health.blocks[2].stale,
                    "observed carrier changes mark other measured health blocks stale");

    if (!passed)
    {
        return 1;
    }
    std::cout << "Ethernet field parser and model tests passed.\n";
    return 0;
}

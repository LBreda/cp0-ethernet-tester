/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

// Ethernet diagnostics: LLDP/CDP neighbor discovery, gateway PTR lookup and link/counter monitoring.

#include "ethernet_diagnostics.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace platform::diagnostics {
namespace {

constexpr char kInterfaceName[] = "eth0";
constexpr auto kLinkMonitorDuration = std::chrono::seconds(5);
constexpr auto kNeighborListenDuration = std::chrono::seconds(5);
constexpr auto kDnsTimeout = std::chrono::milliseconds(3000);
constexpr std::size_t kMaxNeighborFrames = 8;
constexpr std::size_t kMaxDnsPacketBytes = 4096;

model::EthernetTestResult make_result(model::EthernetTestStatus status,
                                      std::string summary,
                                      std::string detail) {
    return {status, std::move(summary), std::move(detail)};
}

uint16_t read_u16(const std::vector<uint8_t>& data, std::size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8U) |
                                  static_cast<uint16_t>(data[offset + 1]));
}

struct EthernetPayload {
    uint16_t type_or_length{0};
    std::size_t offset{0};
};

std::optional<EthernetPayload> ethernet_payload(const std::vector<uint8_t>& frame) {
    if (frame.size() < 14 || frame.size() > 4096) {
        return std::nullopt;
    }
    auto field = read_u16(frame, 12);
    // Payload starts after dst MAC (6) + src MAC (6) + EtherType (2); skip any 802.1Q/802.1ad/QinQ tags (4 bytes each).
    std::size_t offset = 14;
    while (field == 0x8100 || field == 0x88a8 || field == 0x9100) {
        if (frame.size() < offset + 4) {
            return std::nullopt;
        }
        field = read_u16(frame, offset + 2);
        offset += 4;
    }
    return EthernetPayload{field, offset};
}

std::string readable_bytes(const uint8_t* bytes, std::size_t length) {
    if (length == 0) {
        return {};
    }
    const bool printable = std::all_of(bytes, bytes + length, [](uint8_t value) {
        return value >= 0x20 && value <= 0x7e;
    });
    if (printable) {
        return std::string(reinterpret_cast<const char*>(bytes), length);
    }

    std::ostringstream result;
    result << std::hex << std::setfill('0');
    const auto shown = std::min<std::size_t>(length, 24);
    for (std::size_t index = 0; index < shown; ++index) {
        if (index != 0) {
            result << ':';
        }
        result << std::setw(2) << static_cast<unsigned>(bytes[index]);
    }
    if (shown != length) {
        result << ":...";
    }
    return result.str();
}

std::string lldp_identifier(const uint8_t* bytes, std::size_t length, uint8_t subtype) {
    if (subtype == 4 && length == 6) {
        std::ostringstream result;
        result << std::hex << std::setfill('0');
        for (std::size_t index = 0; index < length; ++index) {
            if (index != 0) {
                result << ':';
            }
            result << std::setw(2) << static_cast<unsigned>(bytes[index]);
        }
        return result.str();
    }
    return readable_bytes(bytes, length);
}

std::optional<NeighborAdvertisement> parse_lldp_at(const std::vector<uint8_t>& frame,
                                                   const EthernetPayload& payload) {
    if (payload.type_or_length != 0x88cc || payload.offset >= frame.size()) {
        return std::nullopt;
    }

    NeighborAdvertisement advertisement;
    advertisement.protocol = "LLDP";
    std::size_t offset = payload.offset;
    bool found_end = false;
    while (offset + 2 <= frame.size()) {
        const auto header = read_u16(frame, offset);
        offset += 2;
        // LLDP TLV header: 7-bit type, 9-bit length.
        const auto type = static_cast<uint8_t>(header >> 9U);
        const auto length = static_cast<std::size_t>(header & 0x01ffU);
        if (length > frame.size() - offset) {
            return std::nullopt;
        }
        const auto* value = frame.data() + offset;
        if (type == 0) {
            if (length != 0) {
                return std::nullopt;
            }
            found_end = true;
            break;
        }
        if (type == 1 && length >= 2 && advertisement.chassis_id.empty()) {
            advertisement.chassis_id = lldp_identifier(value + 1, length - 1, value[0]);
        } else if (type == 2 && length >= 2 && advertisement.port_id.empty()) {
            advertisement.port_id = lldp_identifier(value + 1, length - 1, value[0]);
        } else if (type == 4 && length != 0 && advertisement.port_description.empty()) {
            advertisement.port_description = readable_bytes(value, length);
        } else if (type == 5 && length != 0 && advertisement.system_name.empty()) {
            advertisement.system_name = readable_bytes(value, length);
        // Org-specific TLV, IEEE 802.1 OUI 00-80-C2 subtype 1: port VLAN ID.
        } else if (type == 127 && length >= 6 && value[0] == 0x00 && value[1] == 0x80 &&
                   value[2] == 0xc2 && value[3] == 1 && advertisement.vlan.empty()) {
            advertisement.vlan = std::to_string(read_u16(frame, offset + 4));
        }
        offset += length;
    }

    if (!found_end || advertisement.chassis_id.empty() || advertisement.port_id.empty()) {
        return std::nullopt;
    }
    return advertisement;
}

std::optional<NeighborAdvertisement> parse_cdp_at(const std::vector<uint8_t>& frame,
                                                  const EthernetPayload& payload) {
    if (payload.type_or_length >= 1536 || payload.type_or_length < 12 ||
        payload.offset + payload.type_or_length > frame.size()) {
        return std::nullopt;
    }
    const auto end = payload.offset + payload.type_or_length;
    const auto offset = payload.offset;
    // 802.2 LLC/SNAP header with Cisco OUI 00-00-0C and protocol 0x2000 (CDP), followed by the 4-byte CDP header.
    if (frame[offset] != 0xaa || frame[offset + 1] != 0xaa || frame[offset + 2] != 0x03 ||
        frame[offset + 3] != 0x00 || frame[offset + 4] != 0x00 || frame[offset + 5] != 0x0c ||
        frame[offset + 6] != 0x20 || frame[offset + 7] != 0x00 || end < offset + 12) {
        return std::nullopt;
    }

    NeighborAdvertisement advertisement;
    advertisement.protocol = "CDP";
    std::size_t cursor = offset + 12;
    while (cursor + 4 <= end) {
        const auto type = read_u16(frame, cursor);
        const auto length = static_cast<std::size_t>(read_u16(frame, cursor + 2));
        if (length < 4 || length > end - cursor) {
            return std::nullopt;
        }
        const auto value_offset = cursor + 4;
        const auto value_length = length - 4;
        if (type == 0x0001 && value_length != 0 && advertisement.chassis_id.empty()) {
            advertisement.chassis_id = readable_bytes(frame.data() + value_offset, value_length);
        } else if (type == 0x0003 && value_length != 0 && advertisement.port_id.empty()) {
            advertisement.port_id = readable_bytes(frame.data() + value_offset, value_length);
        } else if (type == 0x000a && value_length == 2 && advertisement.vlan.empty()) {
            advertisement.vlan = std::to_string(read_u16(frame, value_offset));
        }
        cursor += length;
    }
    if (cursor != end || advertisement.chassis_id.empty() || advertisement.port_id.empty()) {
        return std::nullopt;
    }
    return advertisement;
}

bool read_dns_name(const std::vector<uint8_t>& packet,
                   std::size_t& offset,
                   std::size_t encoded_end,
                   std::string& name) {
    std::size_t cursor = offset;
    std::size_t consumed = 0;
    bool jumped = false;
    std::vector<std::size_t> visited;
    name.clear();

    for (std::size_t steps = 0; steps < 128; ++steps) {
        if (cursor >= packet.size()) {
            return false;
        }
        const auto length = packet[cursor];
        // DNS name compression pointer: top two bits set, 14-bit offset into the packet.
        if ((length & 0xc0U) == 0xc0U) {
            if (cursor + 1 >= packet.size() || (!jumped && cursor + 2 > encoded_end)) {
                return false;
            }
            const auto target = static_cast<std::size_t>(((length & 0x3fU) << 8U) | packet[cursor + 1]);
            if (target >= packet.size() ||
                std::find(visited.begin(), visited.end(), target) != visited.end()) {
                return false;
            }
            visited.push_back(target);
            if (!jumped) {
                consumed = cursor + 2;
                jumped = true;
            }
            cursor = target;
            continue;
        }
        if ((length & 0xc0U) != 0 || length > 63) {
            return false;
        }
        ++cursor;
        if (length == 0) {
            if (!jumped) {
                consumed = cursor;
            }
            offset = consumed;
            return true;
        }
        if (cursor + length > packet.size() || (!jumped && cursor + length > encoded_end)) {
            return false;
        }
        if (!name.empty()) {
            name.push_back('.');
        }
        for (std::size_t index = 0; index < length; ++index) {
            const auto character = packet[cursor + index];
            name.push_back(character >= 0x21 && character <= 0x7e
                               ? static_cast<char>(character)
                               : '?');
        }
        cursor += length;
    }
    return false;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

#if defined(__linux__)
bool valid_ipv4(const std::string& address, in_addr& parsed) {
    return inet_pton(AF_INET, address.c_str(), &parsed) == 1;
}

std::string read_value(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    input >> value;
    return value;
}

std::optional<uint64_t> read_counter(const char* name) {
    const auto value = read_value(std::filesystem::path("/sys/class/net/eth0/statistics") / name);
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= '0' && character <= '9';
        })) {
        return std::nullopt;
    }
    try {
        return std::stoull(value);
    } catch (...) {
        return std::nullopt;
    }
}

bool read_carrier(bool& carrier) {
    const auto value = read_value("/sys/class/net/eth0/carrier");
    if (value != "0" && value != "1") {
        return false;
    }
    carrier = value == "1";
    return true;
}

std::string ipv4_on_eth0() {
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) {
        return {};
    }
    std::string result;
    for (auto* address = addresses; address; address = address->ifa_next) {
        if (!address->ifa_addr || address->ifa_addr->sa_family != AF_INET ||
            std::string(address->ifa_name) != kInterfaceName) {
            continue;
        }
        char buffer[INET_ADDRSTRLEN]{};
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->ifa_addr);
        if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer))) {
            result = buffer;
            break;
        }
    }
    freeifaddrs(addresses);
    return result;
}

std::string default_gateway_on_eth0() {
    std::ifstream route_table("/proc/net/route");
    std::string line;
    std::getline(route_table, line);
    while (std::getline(route_table, line)) {
        std::istringstream row(line);
        std::string interface_name;
        std::string destination;
        std::string gateway;
        std::string flags;
        if (!(row >> interface_name >> destination >> gateway >> flags) ||
            interface_name != kInterfaceName || destination != "00000000") {
            continue;
        }
        try {
            if ((std::stoul(flags, nullptr, 16) & 0x2UL) == 0) {
                continue;
            }
        } catch (...) {
            continue;
        }
        unsigned long parsed_gateway = 0;
        try {
            parsed_gateway = std::stoul(gateway, nullptr, 16);
        } catch (...) {
            continue;
        }
        if (parsed_gateway == 0) {
            continue;
        }
        in_addr address{};
        address.s_addr = static_cast<in_addr_t>(parsed_gateway);
        char buffer[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &address, buffer, sizeof(buffer))) {
            return buffer;
        }
    }
    return {};
}

std::string configured_dns_resolver() {
    std::ifstream resolv_conf("/etc/resolv.conf");
    std::string line;
    while (std::getline(resolv_conf, line)) {
        std::istringstream row(line);
        std::string directive;
        std::string address;
        if (!(row >> directive >> address) || directive != "nameserver") {
            continue;
        }
        in_addr parsed{};
        if (valid_ipv4(address, parsed)) {
            return address;
        }
    }
    return {};
}

std::string errno_detail(const char* action) {
    return std::string(action) + ": " + std::strerror(errno);
}
#endif

} // namespace

std::optional<NeighborAdvertisement> parse_lldp_frame(const std::vector<uint8_t>& frame) {
    const auto payload = ethernet_payload(frame);
    return payload ? parse_lldp_at(frame, *payload) : std::nullopt;
}

std::optional<NeighborAdvertisement> parse_cdp_frame(const std::vector<uint8_t>& frame) {
    const auto payload = ethernet_payload(frame);
    return payload ? parse_cdp_at(frame, *payload) : std::nullopt;
}

std::optional<NeighborAdvertisement> parse_neighbor_frame(const std::vector<uint8_t>& frame) {
    if (const auto lldp = parse_lldp_frame(frame)) {
        return lldp;
    }
    return parse_cdp_frame(frame);
}

std::optional<std::string> gateway_ptr_query_name(const std::string& gateway) {
#if defined(__linux__)
    in_addr parsed{};
    if (!valid_ipv4(gateway, parsed)) {
        return std::nullopt;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(&parsed.s_addr);
    return std::to_string(bytes[3]) + "." + std::to_string(bytes[2]) + "." +
           std::to_string(bytes[1]) + "." + std::to_string(bytes[0]) + ".in-addr.arpa";
#else
    (void)gateway;
    return std::nullopt;
#endif
}

std::vector<uint8_t> make_ptr_query(const std::string& gateway, uint16_t transaction_id) {
    const auto query_name = gateway_ptr_query_name(gateway);
    if (!query_name) {
        return {};
    }

    std::vector<uint8_t> packet(12, 0);
    packet[0] = static_cast<uint8_t>(transaction_id >> 8U);
    packet[1] = static_cast<uint8_t>(transaction_id & 0xffU);
    packet[2] = 0x01;
    packet[5] = 0x01;
    std::size_t start = 0;
    while (start < query_name->size()) {
        const auto end = query_name->find('.', start);
        const auto length = (end == std::string::npos ? query_name->size() : end) - start;
        if (length == 0 || length > 63 || packet.size() + length + 1 > 255) {
            return {};
        }
        packet.push_back(static_cast<uint8_t>(length));
        for (std::size_t index = 0; index < length; ++index) {
            packet.push_back(static_cast<uint8_t>((*query_name)[start + index]));
        }
        start = end == std::string::npos ? query_name->size() : end + 1;
    }
    packet.push_back(0);
    packet.insert(packet.end(), {0x00, 0x0c, 0x00, 0x01});
    return packet;
}

DnsPtrResponse parse_ptr_response(const std::vector<uint8_t>& packet,
                                  uint16_t transaction_id,
                                  const std::string& expected_query_name) {
    DnsPtrResponse response;
    if (packet.size() < 12 || packet.size() > kMaxDnsPacketBytes || read_u16(packet, 0) != transaction_id) {
        return response;
    }
    const auto flags = read_u16(packet, 2);
    if ((flags & 0x8000U) == 0 || (flags & 0x7800U) != 0 || read_u16(packet, 4) != 1) {
        return response;
    }

    std::size_t cursor = 12;
    std::string question_name;
    if (!read_dns_name(packet, cursor, packet.size(), question_name) || cursor + 4 > packet.size() ||
        lowercase(question_name) != lowercase(expected_query_name) ||
        read_u16(packet, cursor) != 12 || read_u16(packet, cursor + 2) != 1) {
        return response;
    }
    cursor += 4;
    response.valid = true;
    response.truncated = (flags & 0x0200U) != 0;
    response.response_code = static_cast<uint8_t>(flags & 0x000fU);
    if (response.truncated) {
        return response;
    }

    const auto answer_count = read_u16(packet, 6);
    if (answer_count > 256) {
        return {};
    }
    for (uint16_t index = 0; index < answer_count; ++index) {
        std::string owner;
        if (!read_dns_name(packet, cursor, packet.size(), owner) || cursor + 10 > packet.size()) {
            return {};
        }
        const auto type = read_u16(packet, cursor);
        const auto record_class = read_u16(packet, cursor + 2);
        const auto data_length = static_cast<std::size_t>(read_u16(packet, cursor + 8));
        cursor += 10;
        if (data_length > packet.size() - cursor) {
            return {};
        }
        if (type == 12 && record_class == 1) {
            const auto data_end = cursor + data_length;
            auto name_cursor = cursor;
            std::string ptr_name;
            if (!read_dns_name(packet, name_cursor, data_end, ptr_name) || name_cursor > data_end) {
                return {};
            }
            response.ptr_records.push_back(std::move(ptr_name));
        }
        cursor += data_length;
    }
    return response;
}

CounterDelta counter_delta(uint64_t before, uint64_t after) {
    return after >= before ? CounterDelta{after - before, false} : CounterDelta{after, true};
}

model::EthernetTestResult monitor_link(const std::atomic<bool>& cancel_requested) {
#if defined(__linux__)
    const auto interface_path = std::filesystem::path("/sys/class/net") / kInterfaceName;
    if (!std::filesystem::exists(interface_path)) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "eth0 not found",
                           "The link monitor only reads the eth0 Linux interface.");
    }
    if (cancel_requested.load()) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Link monitor cancelled",
                           "No link samples were collected.");
    }

    struct CounterSample {
        const char* label;
        const char* sysfs_name;
        std::optional<uint64_t> before;
    };
    std::array<CounterSample, 5> counters = {{{"RX errors", "rx_errors", std::nullopt},
                                              {"TX errors", "tx_errors", std::nullopt},
                                              {"RX drops", "rx_dropped", std::nullopt},
                                              {"TX drops", "tx_dropped", std::nullopt},
                                              {"RX CRC errors", "rx_crc_errors", std::nullopt}}};
    for (auto& counter : counters) {
        counter.before = read_counter(counter.sysfs_name);
    }

    bool initial_carrier = false;
    if (!read_carrier(initial_carrier)) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "Link status unavailable",
                           "The driver does not expose /sys/class/net/eth0/carrier.");
    }
    bool last_carrier = initial_carrier;
    unsigned observed_transitions = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + kLinkMonitorDuration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_requested.load()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bool carrier = false;
        if (read_carrier(carrier) && carrier != last_carrier) {
            ++observed_transitions;
            last_carrier = carrier;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    const bool cancelled = cancel_requested.load();
    bool final_carrier = last_carrier;
    const bool final_carrier_available = read_carrier(final_carrier);
    const auto state = read_value(interface_path / "operstate");
    const auto speed = read_value(interface_path / "speed");
    const auto duplex = read_value(interface_path / "duplex");

    std::ostringstream detail;
    detail << "Interface: eth0\nSample: " << elapsed.count() << " ms"
           << "\nCarrier start: " << (initial_carrier ? "up" : "down")
           << "\nCarrier end: "
           << (final_carrier_available ? (final_carrier ? "up" : "down") : "unavailable")
           << "\nObserved carrier changes: " << observed_transitions
           << "\nState: " << (state.empty() ? "unavailable" : state)
           << "\nSpeed: " << (speed.empty() || speed == "-1" ? "unavailable" : speed + " Mb/s")
           << "\nDuplex: " << (duplex.empty() ? "unavailable" : duplex);
    for (const auto& counter : counters) {
        detail << '\n' << counter.label << " delta: ";
        const auto after = read_counter(counter.sysfs_name);
        if (!counter.before || !after) {
            detail << "unavailable";
        } else {
            const auto difference = counter_delta(*counter.before, *after);
            if (difference.reset) {
                detail << "counter reset; final " << difference.value;
            } else {
                detail << difference.value;
            }
        }
    }
    detail << "\nA clean sample does not certify the cable; short carrier flaps may be missed.";

    if (cancelled) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Link monitor cancelled",
                           detail.str());
    }
    if (!final_carrier_available) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "Link status unavailable",
                           detail.str());
    }
    return make_result(final_carrier ? model::EthernetTestStatus::Passed
                                     : model::EthernetTestStatus::Failed,
                       final_carrier ? "Link sample complete" : "Carrier is down",
                       detail.str());
#else
    (void)cancel_requested;
    return make_result(model::EthernetTestStatus::Unsupported,
                       "Link monitor unavailable",
                       "Temporal link diagnostics are supported only on Linux.");
#endif
}

model::EthernetTestResult listen_for_neighbors(const std::atomic<bool>& cancel_requested) {
#if defined(__linux__)
    if (!std::filesystem::exists(std::filesystem::path("/sys/class/net") / kInterfaceName)) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "eth0 not found",
                           "LLDP/CDP listening is limited to eth0.");
    }
    if (cancel_requested.load()) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Neighbor listen cancelled",
                           "No packet socket was opened.");
    }
    const int socket_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (socket_fd < 0) {
        const auto permission_denied = errno == EACCES || errno == EPERM;
        return make_result(permission_denied ? model::EthernetTestStatus::Unsupported
                                             : model::EthernetTestStatus::Error,
                            permission_denied ? "Packet capture permission required"
                                              : "Could not open packet socket",
                            permission_denied
                                ? "Switch listening needs CAP_NET_RAW. The Debian package sets this on the app; after a manual install run:\nsudo setcap cap_net_raw+ep /usr/bin/ethernet-tester"
                                : errno_detail("AF_PACKET socket"));
    }

    sockaddr_ll address{};
    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(ETH_P_ALL);
    address.sll_ifindex = static_cast<int>(if_nametoindex(kInterfaceName));
    if (address.sll_ifindex == 0 || bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const auto error = errno_detail("Binding packet socket to eth0");
        close(socket_fd);
        return make_result(model::EthernetTestStatus::Error,
                           "Could not listen on eth0",
                           error);
    }

    std::vector<NeighborAdvertisement> advertisements;
    std::size_t additional_count = 0;
    const auto deadline = std::chrono::steady_clock::now() + kNeighborListenDuration;
    bool cancelled = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_requested.load()) {
            cancelled = true;
            break;
        }
        pollfd descriptor{socket_fd, POLLIN, 0};
        const auto ready = poll(&descriptor, 1, 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto error = errno_detail("Waiting for neighbor advertisements");
            close(socket_fd);
            return make_result(model::EthernetTestStatus::Error,
                               "Neighbor listen failed",
                               error);
        }
        if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        std::array<uint8_t, 4096> frame{};
        const auto count = recv(socket_fd, frame.data(), frame.size(), 0);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            const auto error = errno_detail("Reading neighbor frame");
            close(socket_fd);
            return make_result(model::EthernetTestStatus::Error,
                               "Neighbor listen failed",
                               error);
        }
        const std::vector<uint8_t> bytes(frame.begin(), frame.begin() + count);
        const auto parsed = parse_neighbor_frame(bytes);
        if (!parsed) {
            continue;
        }
        const auto duplicate = std::find_if(advertisements.begin(), advertisements.end(),
                                            [&parsed](const NeighborAdvertisement& existing) {
                                                return existing.protocol == parsed->protocol &&
                                                       existing.chassis_id == parsed->chassis_id &&
                                                       existing.port_id == parsed->port_id;
                                            });
        if (duplicate != advertisements.end()) {
            continue;
        }
        if (advertisements.size() < kMaxNeighborFrames) {
            advertisements.push_back(*parsed);
        } else {
            ++additional_count;
        }
    }
    close(socket_fd);

    std::ostringstream detail;
    detail << "Interface: eth0\nListen window: 5 s\nAnnouncements shown: " << advertisements.size();
    if (additional_count != 0) {
        detail << " (" << additional_count << " additional unique announcements omitted)";
    }
    if (advertisements.empty()) {
        detail << "\nNo LLDP/CDP advertisement was observed; the switch may not announce either protocol.";
    }
    for (std::size_t index = 0; index < advertisements.size(); ++index) {
        const auto& advertisement = advertisements[index];
        detail << "\n\nNeighbor " << (index + 1) << " (" << advertisement.protocol << ')';
        if (!advertisement.chassis_id.empty()) detail << "\nChassis: " << advertisement.chassis_id;
        if (!advertisement.port_id.empty()) detail << "\nPort: " << advertisement.port_id;
        if (!advertisement.system_name.empty()) detail << "\nSystem: " << advertisement.system_name;
        if (!advertisement.port_description.empty()) {
            detail << "\nPort description: " << advertisement.port_description;
        }
        if (!advertisement.vlan.empty()) detail << "\nVLAN: " << advertisement.vlan;
    }
    detail << "\nOnly fields included in received announcements are shown; this is passive and sends no frames.";
    return make_result(cancelled ? model::EthernetTestStatus::Cancelled
                                 : model::EthernetTestStatus::Passed,
                       cancelled ? "Neighbor listen cancelled"
                                 : advertisements.empty() ? "Listen complete" : "Switch announcements received",
                       detail.str());
#else
    (void)cancel_requested;
    return make_result(model::EthernetTestStatus::Unsupported,
                       "Neighbor listen unavailable",
                       "Passive LLDP/CDP listening is supported only on Linux.");
#endif
}

model::EthernetTestResult lookup_gateway_ptr(const std::atomic<bool>& cancel_requested) {
#if defined(__linux__)
    if (!std::filesystem::exists(std::filesystem::path("/sys/class/net") / kInterfaceName)) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "eth0 not found",
                           "The DNS check is limited to the configured eth0 gateway.");
    }
    const auto gateway = default_gateway_on_eth0();
    const auto query_name = gateway_ptr_query_name(gateway);
    if (!query_name) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "No eth0 gateway",
                           "Assign a default IPv4 route to eth0 before checking its reverse DNS record.");
    }
    const auto resolver = configured_dns_resolver();
    in_addr resolver_address{};
    if (resolver.empty() || !valid_ipv4(resolver, resolver_address)) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "No IPv4 DNS resolver",
                           "No IPv4 nameserver is configured in /etc/resolv.conf.");
    }
    if ((ntohl(resolver_address.s_addr) & 0xff000000U) == 0x7f000000U) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "Local DNS stub cannot use eth0",
                           "The configured resolver is loopback-only; an eth0 upstream resolver is required.");
    }
    const auto source = ipv4_on_eth0();
    in_addr source_address{};
    if (source.empty() || !valid_ipv4(source, source_address)) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "No IPv4 address on eth0",
                           "The DNS query requires an IPv4 source address on eth0.");
    }
    if (cancel_requested.load()) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "DNS check cancelled",
                           "No DNS query was sent.");
    }

    static std::atomic<uint16_t> next_transaction{0x4200};
    const auto transaction_id = static_cast<uint16_t>(next_transaction.fetch_add(1) ^
        static_cast<uint16_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto query = make_ptr_query(gateway, transaction_id);
    if (query.empty()) {
        return make_result(model::EthernetTestStatus::Error,
                           "Could not build DNS query",
                           "The configured gateway did not produce a valid PTR query name.");
    }

    const int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (socket_fd < 0) {
        return make_result(model::EthernetTestStatus::Error,
                           "Could not open DNS socket",
                           errno_detail("UDP socket"));
    }
    if (setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, kInterfaceName, sizeof(kInterfaceName)) != 0) {
        const auto permission_denied = errno == EPERM || errno == EACCES;
        const auto error = errno_detail("Binding DNS socket to eth0");
        close(socket_fd);
        return make_result(permission_denied ? model::EthernetTestStatus::Unsupported
                                             : model::EthernetTestStatus::Error,
                           permission_denied ? "Interface binding permission required"
                                             : "Could not bind DNS socket",
                           error);
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr = source_address;
    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        const auto error = errno_detail("Binding DNS source address");
        close(socket_fd);
        return make_result(model::EthernetTestStatus::Error,
                           "Could not bind DNS socket",
                           error);
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(53);
    destination.sin_addr = resolver_address;
    const auto sent = sendto(socket_fd, query.data(), query.size(), 0,
                             reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    if (sent != static_cast<ssize_t>(query.size())) {
        const auto error = errno_detail("Sending PTR query");
        close(socket_fd);
        return make_result(model::EthernetTestStatus::Error,
                           "Could not send DNS query",
                           error);
    }

    const auto deadline = std::chrono::steady_clock::now() + kDnsTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel_requested.load()) {
            close(socket_fd);
            return make_result(model::EthernetTestStatus::Cancelled,
                               "DNS check cancelled",
                               "The single PTR query was sent; the response wait was stopped.");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd descriptor{socket_fd, POLLIN, 0};
        const auto timeout = std::max<int64_t>(0, std::min<int64_t>(100, remaining.count()));
        const auto ready = poll(&descriptor, 1, static_cast<int>(timeout));
        if (ready < 0) {
            if (errno == EINTR) continue;
            const auto error = errno_detail("Waiting for DNS response");
            close(socket_fd);
            return make_result(model::EthernetTestStatus::Error,
                               "DNS check failed",
                               error);
        }
        if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        std::array<uint8_t, kMaxDnsPacketBytes> buffer{};
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const auto received = recvfrom(socket_fd, buffer.data(), buffer.size(), 0,
                                       reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            const auto error = errno_detail("Reading DNS response");
            close(socket_fd);
            return make_result(model::EthernetTestStatus::Error,
                               "DNS check failed",
                               error);
        }
        if (peer.sin_addr.s_addr != resolver_address.s_addr) {
            continue;
        }
        const std::vector<uint8_t> packet(buffer.begin(), buffer.begin() + received);
        const auto response = parse_ptr_response(packet, transaction_id, *query_name);
        close(socket_fd);
        if (!response.valid) {
            return make_result(model::EthernetTestStatus::Failed,
                               "Invalid DNS response",
                               "The configured resolver returned a response that did not match the PTR query.");
        }
        if (response.truncated) {
            return make_result(model::EthernetTestStatus::Unsupported,
                               "DNS response needs TCP",
                               "The UDP PTR response was truncated; this bounded test does not retry over TCP.");
        }
        if (response.response_code != 0) {
            return make_result(model::EthernetTestStatus::Failed,
                               "DNS server returned an error",
                               "Resolver: " + resolver + "\nPTR query: " + *query_name +
                                   "\nDNS response code: " + std::to_string(response.response_code));
        }
        std::ostringstream detail;
        detail << "Gateway: " << gateway << "\nPTR query: " << *query_name
               << "\nResolver: " << resolver << "\nInterface: eth0";
        if (response.ptr_records.empty()) {
            detail << "\nDNS replied successfully; no PTR record was returned for this gateway.";
        } else {
            for (const auto& record : response.ptr_records) {
                detail << "\nPTR: " << record;
            }
        }
        detail << "\nOnly the configured gateway reverse record was queried.";
        return make_result(model::EthernetTestStatus::Passed,
                           response.ptr_records.empty() ? "DNS replied; no PTR record"
                                                        : "Gateway PTR resolved",
                           detail.str());
    }
    close(socket_fd);
    return make_result(model::EthernetTestStatus::Failed,
                       "DNS response timed out",
                       "One PTR query to " + resolver + " exceeded the 3 second limit.");
#else
    (void)cancel_requested;
    return make_result(model::EthernetTestStatus::Unsupported,
                       "DNS check unavailable",
                       "The gateway PTR check is supported only on Linux.");
#endif
}

} // namespace platform::diagnostics

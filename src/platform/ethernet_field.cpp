/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

// DHCP lease file parsing and passive capture/aggregation of ARP, DHCP offer and STP frames.

#include "ethernet_field.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace platform::field {
namespace {

constexpr char kInterfaceName[] = "eth0";
constexpr auto kListenDuration = std::chrono::seconds(5);
constexpr std::size_t kMaxFramesRead = 512;
constexpr std::size_t kMaxFrameBytes = 4096;
constexpr auto kNetworkManagerTimeout = std::chrono::seconds(2);

#if defined(__linux__)
void terminate_and_reap(pid_t child, int& wait_status) {
    kill(child, SIGTERM);
    const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < grace_deadline) {
        const auto waited = waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            return;
        }
        if (waited < 0 && errno != EINTR) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(child, SIGKILL);
    while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
    }
}

std::optional<std::string> run_nmcli(const std::string& interface_name) {
    constexpr std::array<const char*, 2> executable_paths = {"/usr/bin/nmcli", "/bin/nmcli"};
    const char* executable = nullptr;
    for (const auto* path : executable_paths) {
        if (access(path, X_OK) == 0) {
            executable = path;
            break;
        }
    }
    if (!executable) {
        return std::nullopt;
    }

    std::array<char*, 10> arguments = {
        const_cast<char*>(executable),
        const_cast<char*>("--terse"),
        const_cast<char*>("--escape"),
        const_cast<char*>("no"),
        const_cast<char*>("--fields"),
        const_cast<char*>("GENERAL.DEVICE,IP4.ADDRESS,DHCP4.OPTION"),
        const_cast<char*>("device"),
        const_cast<char*>("show"),
        const_cast<char*>(interface_name.c_str()),
        nullptr,
    };
    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) {
        return std::nullopt;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return std::nullopt;
    }
    if (child == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        execv(executable, arguments.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    const auto flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        int wait_status = 0;
        terminate_and_reap(child, wait_status);
        close(pipe_fds[0]);
        return std::nullopt;
    }

    std::string output;
    output.reserve(kMaxLeaseBytes);
    const auto deadline = std::chrono::steady_clock::now() + kNetworkManagerTimeout;
    bool child_exited = false;
    bool pipe_closed = false;
    bool failed = false;
    int wait_status = 0;
    while (std::chrono::steady_clock::now() < deadline && (!child_exited || !pipe_closed)) {
        pollfd descriptor{pipe_fds[0], POLLIN | POLLHUP, 0};
        const auto ready = poll(&descriptor, 1, 50);
        if (ready < 0 && errno != EINTR) {
            failed = true;
            break;
        }
        if (ready > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            while (true) {
                std::array<char, 2048> buffer{};
                const auto count = read(pipe_fds[0], buffer.data(), buffer.size());
                if (count > 0) {
                    const auto bytes = static_cast<std::size_t>(count);
                    if (output.size() + bytes > kMaxLeaseBytes) {
                        failed = true;
                        break;
                    }
                    output.append(buffer.data(), bytes);
                    continue;
                }
                if (count == 0) {
                    pipe_closed = true;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    failed = true;
                }
                break;
            }
            if (failed) {
                break;
            }
        } else if (ready > 0 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            failed = true;
            break;
        }

        const auto waited = waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            child_exited = true;
        } else if (waited < 0 && errno != EINTR) {
            failed = true;
            break;
        }
    }

    if (!child_exited) {
        failed = true;
    }
    if (failed) {
        if (!child_exited) {
            terminate_and_reap(child, wait_status);
        }
        close(pipe_fds[0]);
        return std::nullopt;
    }
    close(pipe_fds[0]);
    if (!pipe_closed || !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        return std::nullopt;
    }
    return output;
}
#endif

bool valid_ipv4(const std::string& address) {
    std::size_t start = 0;
    for (std::size_t part = 0; part < 4; ++part) {
        const auto end = address.find('.', start);
        const auto length = (end == std::string::npos ? address.size() : end) - start;
        if (length == 0 || length > 3 || (length > 1 && address[start] == '0')) {
            return false;
        }
        unsigned value = 0;
        for (std::size_t index = start; index < start + length; ++index) {
            const auto digit = address[index];
            if (digit < '0' || digit > '9') {
                return false;
            }
            value = value * 10U + static_cast<unsigned>(digit - '0');
        }
        if (value > 255U || (part < 3 && end == std::string::npos) ||
            (part == 3 && end != std::string::npos)) {
            return false;
        }
        start = end == std::string::npos ? address.size() : end + 1;
    }
    return start == address.size();
}

std::optional<std::string> valid_ipv4_prefix(const std::string& value) {
    const auto slash = value.find('/');
    const auto address = value.substr(0, slash);
    if (!valid_ipv4(address)) {
        return std::nullopt;
    }
    if (slash != std::string::npos) {
        unsigned prefix = 0;
        const auto* begin = value.data() + slash + 1;
        const auto* end = value.data() + value.size();
        const auto parsed = std::from_chars(begin, end, prefix);
        if (parsed.ec != std::errc{} || parsed.ptr != end || prefix > 32) {
            return std::nullopt;
        }
    }
    return address;
}

std::optional<uint32_t> parse_seconds(const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }
    uint32_t seconds = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return seconds;
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    value.erase(0, first);
    return value;
}

bool starts_with(const std::string& value, const char* prefix) {
    const auto prefix_length = std::char_traits<char>::length(prefix);
    return value.size() >= prefix_length && value.compare(0, prefix_length, prefix) == 0;
}

model::EthernetTestResult make_result(model::EthernetTestStatus status,
                                      std::string summary,
                                      std::string detail) {
    return {status, std::move(summary), std::move(detail)};
}

struct EthernetPayload {
    uint16_t type_or_length{0};
    std::size_t offset{0};
};

uint16_t read_u16(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8U) |
                                 static_cast<uint16_t>(bytes[offset + 1]));
}

std::optional<EthernetPayload> ethernet_payload(const std::vector<uint8_t>& frame) {
    if (frame.size() < 14 || frame.size() > kMaxFrameBytes) {
        return std::nullopt;
    }
    auto type_or_length = read_u16(frame, 12);
    std::size_t offset = 14;
    while (type_or_length == 0x8100 || type_or_length == 0x88a8 || type_or_length == 0x9100) {
        if (frame.size() - offset < 4) {
            return std::nullopt;
        }
        type_or_length = read_u16(frame, offset + 2);
        offset += 4;
    }
    return EthernetPayload{type_or_length, offset};
}

std::string ipv4_text(const uint8_t* bytes) {
    return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
           std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}

std::string mac_text(const uint8_t* bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(17);
    for (std::size_t index = 0; index < 6; ++index) {
        if (index != 0) {
            result.push_back(':');
        }
        result.push_back(digits[bytes[index] >> 4U]);
        result.push_back(digits[bytes[index] & 0x0fU]);
    }
    return result;
}

std::optional<PacketObservation> parse_arp(const std::vector<uint8_t>& frame,
                                           const EthernetPayload& payload) {
    constexpr std::size_t kArpHeaderLength = 8;
    constexpr std::size_t kEthernetIpv4ArpLength = kArpHeaderLength + 6 + 4 + 6 + 4;
    if (payload.type_or_length != ETH_P_ARP || frame.size() - payload.offset < kEthernetIpv4ArpLength) {
        return std::nullopt;
    }
    const auto offset = payload.offset;
    // Require Ethernet hardware type, IPv4 protocol, 6-byte MAC and 4-byte IPv4 addresses.
    if (read_u16(frame, offset) != 1 || read_u16(frame, offset + 2) != ETH_P_IP ||
        frame[offset + 4] != 6 || frame[offset + 5] != 4) {
        return std::nullopt;
    }
    const auto operation = read_u16(frame, offset + 6);
    if (operation != 1 && operation != 2) {
        return std::nullopt;
    }

    PacketObservation observation;
    observation.protocol = Protocol::Arp;
    // ARP body: sender MAC, sender IPv4, target MAC, target IPv4.
    observation.source_mac = mac_text(frame.data() + offset + kArpHeaderLength);
    observation.ipv4 = ipv4_text(frame.data() + offset + kArpHeaderLength + 6);
    observation.related_ipv4 = ipv4_text(frame.data() + offset + kArpHeaderLength + 6 + 4 + 6);
    return observation;
}

std::optional<PacketObservation> parse_dhcp_offer(const std::vector<uint8_t>& frame,
                                                  const EthernetPayload& payload) {
    if (payload.type_or_length != ETH_P_IP || frame.size() - payload.offset < 20) {
        return std::nullopt;
    }
    const auto ip_offset = payload.offset;
    const auto version_and_length = frame[ip_offset];
    const auto ip_header_length = static_cast<std::size_t>(version_and_length & 0x0fU) * 4;
    if ((version_and_length >> 4U) != 4 || ip_header_length < 20 ||
        frame.size() - ip_offset < ip_header_length || frame[ip_offset + 9] != 17) {
        return std::nullopt;
    }
    const auto total_length = static_cast<std::size_t>(read_u16(frame, ip_offset + 2));
    // Reject fragments (MF flag or non-zero fragment offset).
    const auto fragment = read_u16(frame, ip_offset + 6);
    if (total_length < ip_header_length + 8 || total_length > frame.size() - ip_offset ||
        (fragment & 0x3fffU) != 0) {
        return std::nullopt;
    }
    const auto udp_offset = ip_offset + ip_header_length;
    const auto udp_length = static_cast<std::size_t>(read_u16(frame, udp_offset + 4));
    const auto source_port = read_u16(frame, udp_offset);
    const auto destination_port = read_u16(frame, udp_offset + 2);
    if (udp_length < 8 + 240 || udp_length > total_length - ip_header_length ||
        !((source_port == 67 && destination_port == 68) ||
          (source_port == 68 && destination_port == 67))) {
        return std::nullopt;
    }
    const auto bootp_offset = udp_offset + 8;
    const auto bootp_length = udp_length - 8;
    // BOOTREPLY op code and DHCP magic cookie 63 82 53 63 after the 236-byte BOOTP header.
    if (frame[bootp_offset] != 2 || bootp_length < 240 || frame[bootp_offset + 236] != 0x63 ||
        frame[bootp_offset + 237] != 0x82 || frame[bootp_offset + 238] != 0x53 ||
        frame[bootp_offset + 239] != 0x63) {
        return std::nullopt;
    }

    uint8_t message_type = 0;
    std::string server_identifier;
    bool ended = false;
    std::size_t option_offset = bootp_offset + 240;
    const auto options_end = bootp_offset + bootp_length;
    while (option_offset < options_end) {
        const auto option = frame[option_offset++];
        if (option == 0) {
            continue;
        }
        if (option == 255) {
            ended = true;
            break;
        }
        if (option_offset >= options_end) {
            return std::nullopt;
        }
        const auto option_length = static_cast<std::size_t>(frame[option_offset++]);
        if (option_length > options_end - option_offset) {
            return std::nullopt;
        }
        // Option 53: DHCP message type (2 = OFFER); option 54: server identifier.
        if (option == 53) {
            if (option_length != 1) {
                return std::nullopt;
            }
            message_type = frame[option_offset];
        } else if (option == 54) {
            if (option_length != 4) {
                return std::nullopt;
            }
            server_identifier = ipv4_text(frame.data() + option_offset);
        }
        option_offset += option_length;
    }
    if (!ended || message_type != 2) {
        return std::nullopt;
    }

    PacketObservation observation;
    observation.protocol = Protocol::DhcpOffer;
    observation.source_mac = mac_text(frame.data() + 6);
    observation.server_ipv4 = server_identifier.empty()
                                  ? ipv4_text(frame.data() + ip_offset + 12)
                                  : std::move(server_identifier);
    observation.offered_ipv4 = ipv4_text(frame.data() + bootp_offset + 16);
    return observation;
}

std::optional<PacketObservation> parse_stp(const std::vector<uint8_t>& frame,
                                           const EthernetPayload& payload) {
    if (payload.type_or_length < 7 || payload.type_or_length > 1500 ||
        payload.type_or_length > frame.size() - payload.offset) {
        return std::nullopt;
    }
    const auto offset = payload.offset;
    if (frame[offset] != 0x42 || frame[offset + 1] != 0x42 || frame[offset + 2] != 0x03 ||
        frame[offset + 3] != 0 || frame[offset + 4] != 0) {
        return std::nullopt;
    }
    PacketObservation observation;
    observation.protocol = Protocol::Stp;
    observation.source_mac = mac_text(frame.data() + 6);
    observation.bpdu_version = frame[offset + 5];
    observation.bpdu_type = frame[offset + 6];
    if (observation.bpdu_type != 0 && observation.bpdu_type != 0x80 &&
        observation.bpdu_type != 2) {
        return std::nullopt;
    }
    return observation;
}

std::string bpdu_label(uint8_t version, uint8_t type) {
    const char* type_name = type == 0 ? "Configuration" : type == 0x80 ? "Topology change" : "RSTP/MSTP";
    return std::string(type_name) + " BPDU v" + std::to_string(version);
}

void append_list(std::ostringstream& output, const std::vector<std::string>& values) {
    for (const auto& value : values) {
        output << "\n" << value;
    }
}

#if defined(__linux__)
bool attach_field_filter(int socket_fd) {
    sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_ARP, 3, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_IP, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021Q, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, ETH_P_8021AD, 0, 1),
        BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 1500, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, static_cast<uint32_t>(kMaxFrameBytes)),
        BPF_STMT(BPF_RET | BPF_K, 0),
    };
    sock_fprog program{};
    program.len = static_cast<unsigned short>(sizeof(instructions) / sizeof(instructions[0]));
    program.filter = instructions;
    return setsockopt(socket_fd, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) == 0;
}

std::string errno_detail(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}
#endif

} // namespace

std::optional<DhcpLease> parse_networkd_lease(const std::string& contents) {
    if (contents.empty() || contents.size() > kMaxLeaseBytes) {
        return std::nullopt;
    }
    DhcpLease lease;
    std::size_t cursor = 0;
    while (cursor < contents.size()) {
        const auto end = contents.find('\n', cursor);
        auto line = contents.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? contents.size() : end + 1;
        if (line.size() > 1024) {
            return std::nullopt;
        }
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        const auto key = line.substr(0, separator);
        const auto value = trim(line.substr(separator + 1));
        if (key == "ADDRESS") {
            const auto address = valid_ipv4_prefix(value);
            if (address) {
                lease.address = *address;
            }
        } else if (key == "ROUTER") {
            const auto router = valid_ipv4_prefix(value);
            if (router) {
                lease.router = *router;
            }
        } else if (key == "SERVER_ADDRESS") {
            const auto server = valid_ipv4_prefix(value);
            if (server) {
                lease.server_address = *server;
            }
        } else if (key == "DNS") {
            std::istringstream addresses(value);
            std::string address;
            while (addresses >> address) {
                const auto parsed = valid_ipv4_prefix(address);
                if (parsed && std::find(lease.dns_servers.begin(), lease.dns_servers.end(), *parsed) ==
                                  lease.dns_servers.end()) {
                    lease.dns_servers.push_back(*parsed);
                }
            }
        } else if (key == "LIFETIME") {
            lease.lifetime_seconds = parse_seconds(value);
        } else if (key == "T1") {
            lease.renewal_seconds = parse_seconds(value);
        } else if (key == "T2") {
            lease.rebinding_seconds = parse_seconds(value);
        }
    }
    return lease.address.empty() ? std::nullopt : std::optional<DhcpLease>(std::move(lease));
}

std::optional<DhcpLease> parse_networkmanager_lease(const std::string& contents,
                                                    const std::string& interface_name,
                                                    const std::string& current_address) {
    const auto expected_address = valid_ipv4_prefix(current_address);
    if (contents.empty() || contents.size() > kMaxLeaseBytes || interface_name.empty() ||
        !expected_address) {
        return std::nullopt;
    }

    DhcpLease lease;
    std::set<std::string> interface_addresses;
    bool interface_found = false;
    std::size_t cursor = 0;
    while (cursor < contents.size()) {
        const auto end = contents.find('\n', cursor);
        auto line = contents.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        cursor = end == std::string::npos ? contents.size() : end + 1;
        if (line.size() > 1024) {
            return std::nullopt;
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        const auto separator = line.find(':');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        const auto field_name = line.substr(0, separator);
        const auto value = trim(line.substr(separator + 1));
        if (field_name == "GENERAL.DEVICE") {
            if (value != interface_name) {
                return std::nullopt;
            }
            interface_found = true;
        } else if (starts_with(field_name, "IP4.ADDRESS[")) {
            const auto address = valid_ipv4_prefix(value);
            if (address) {
                interface_addresses.insert(*address);
            }
        } else if (starts_with(field_name, "DHCP4.OPTION[")) {
            const auto option_separator = value.find('=');
            if (option_separator == std::string::npos || option_separator == 0) {
                continue;
            }
            const auto key = trim(value.substr(0, option_separator));
            const auto option_value = trim(value.substr(option_separator + 1));
            if (key == "ip_address") {
                const auto address = valid_ipv4_prefix(option_value);
                if (!address || (!lease.address.empty() && lease.address != *address)) {
                    return std::nullopt;
                }
                lease.address = *address;
            } else if (key == "routers") {
                std::string addresses = option_value;
                std::replace(addresses.begin(), addresses.end(), ',', ' ');
                std::istringstream values(addresses);
                std::string address;
                while (values >> address) {
                    const auto parsed = valid_ipv4_prefix(address);
                    if (parsed) {
                        lease.router = *parsed;
                        break;
                    }
                }
            } else if (key == "dhcp_server_identifier") {
                const auto address = valid_ipv4_prefix(option_value);
                if (address) {
                    lease.server_address = *address;
                }
            } else if (key == "domain_name_servers") {
                std::string addresses = option_value;
                std::replace(addresses.begin(), addresses.end(), ',', ' ');
                std::istringstream values(addresses);
                std::string address;
                while (values >> address) {
                    const auto parsed = valid_ipv4_prefix(address);
                    if (parsed && std::find(lease.dns_servers.begin(), lease.dns_servers.end(), *parsed) ==
                                      lease.dns_servers.end()) {
                        lease.dns_servers.push_back(*parsed);
                    }
                }
            } else if (key == "dhcp_lease_time") {
                lease.lifetime_seconds = parse_seconds(option_value);
            }
        }
    }

    if (!interface_found || lease.address.empty() || lease.address != *expected_address ||
        interface_addresses.find(lease.address) == interface_addresses.end()) {
        return std::nullopt;
    }
    return lease;
}

std::optional<DhcpLease> read_systemd_networkd_lease(const std::string& interface_name) {
#if defined(__linux__)
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ) {
        return std::nullopt;
    }
    const auto interface_index = if_nametoindex(interface_name.c_str());
    if (interface_index == 0) {
        return std::nullopt;
    }
    const auto path = std::filesystem::path("/run/systemd/netif/leases") /
                      std::to_string(interface_index);
    std::ifstream lease_file(path, std::ios::binary);
    if (!lease_file) {
        return std::nullopt;
    }
    std::array<char, kMaxLeaseBytes + 1> buffer{};
    lease_file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = lease_file.gcount();
    if (count < 0 || static_cast<std::size_t>(count) > kMaxLeaseBytes ||
        (!lease_file.eof() && lease_file.fail())) {
        return std::nullopt;
    }
    return parse_networkd_lease(std::string(buffer.data(), static_cast<std::size_t>(count)));
#else
    (void)interface_name;
    return std::nullopt;
#endif
}

std::optional<DhcpLease> read_networkmanager_lease(const std::string& interface_name,
                                                    const std::string& current_address) {
#if defined(__linux__)
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ ||
        if_nametoindex(interface_name.c_str()) == 0 || !valid_ipv4_prefix(current_address)) {
        return std::nullopt;
    }
    const auto output = run_nmcli(interface_name);
    if (!output) {
        return std::nullopt;
    }
    return parse_networkmanager_lease(*output, interface_name, current_address);
#else
    (void)interface_name;
    (void)current_address;
    return std::nullopt;
#endif
}

std::optional<PacketObservation> parse_ethernet_frame(const std::vector<uint8_t>& frame) {
    const auto payload = ethernet_payload(frame);
    if (!payload) {
        return std::nullopt;
    }
    if (const auto arp = parse_arp(frame, *payload)) {
        return arp;
    }
    if (const auto offer = parse_dhcp_offer(frame, *payload)) {
        return offer;
    }
    return parse_stp(frame, *payload);
}

void ObservationAccumulator::ingest(const PacketObservation& observation) {
    if (frames_recorded_ >= kMaxObservedFrames) {
        incomplete_ = true;
        return;
    }
    ++frames_recorded_;
    switch (observation.protocol) {
        case Protocol::Arp:
            ++arp_frames_;
            if (!observation.ipv4.empty() && !observation.source_mac.empty()) {
                arp_macs_[observation.ipv4].insert(observation.source_mac);
            }
            break;
        case Protocol::DhcpOffer:
            ++dhcp_offer_frames_;
            if (!observation.server_ipv4.empty()) {
                dhcp_servers_.insert(observation.server_ipv4);
            }
            break;
        case Protocol::Stp:
            ++stp_frames_;
            stp_bpdus_.insert(bpdu_label(observation.bpdu_version, observation.bpdu_type) +
                              " from " + observation.source_mac);
            break;
    }
}

void ObservationAccumulator::mark_incomplete() {
    incomplete_ = true;
}

FieldSummary ObservationAccumulator::summary() const {
    FieldSummary result;
    result.frames_recorded = frames_recorded_;
    result.arp_frames = arp_frames_;
    result.dhcp_offer_frames = dhcp_offer_frames_;
    result.stp_frames = stp_frames_;
    result.incomplete = incomplete_;
    for (const auto& [address, macs] : arp_macs_) {
        AddressObservation sender;
        sender.address = address;
        sender.mac_addresses.assign(macs.begin(), macs.end());
        if (macs.size() > 1) {
            std::ostringstream conflict;
            conflict << address << " announced by multiple MAC addresses";
            result.possible_ip_conflicts.push_back(conflict.str());
        }
        result.arp_senders.push_back(std::move(sender));
    }
    result.dhcp_servers.assign(dhcp_servers_.begin(), dhcp_servers_.end());
    result.stp_bpdus.assign(stp_bpdus_.begin(), stp_bpdus_.end());
    return result;
}

model::EthernetTestResult listen_for_field_events(const std::atomic<bool>& cancel_requested) {
#if defined(__linux__)
    if (!std::filesystem::exists(std::filesystem::path("/sys/class/net") / kInterfaceName)) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "eth0 not found",
                           "Passive ARP/DHCP/STP listening is limited to eth0.");
    }
    if (cancel_requested.load()) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Listen cancelled",
                           "No packet socket was opened.");
    }

    const int socket_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (socket_fd < 0) {
        const auto permission_denied = errno == EACCES || errno == EPERM;
        return make_result(permission_denied ? model::EthernetTestStatus::Unsupported
                                             : model::EthernetTestStatus::Error,
                           permission_denied ? "Packet listen permission required"
                                            : "Could not open packet socket",
                           permission_denied
                               ? "Passive listening needs CAP_NET_RAW or root; privileges are never elevated automatically."
                               : errno_detail("AF_PACKET socket"));
    }
    if (!attach_field_filter(socket_fd)) {
        const auto error = errno_detail("Attaching ARP/DHCP/STP packet filter");
        close(socket_fd);
        return make_result(model::EthernetTestStatus::Error,
                           "Could not filter passive listen",
                           error);
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

    ObservationAccumulator observations;
    const auto deadline = std::chrono::steady_clock::now() + kListenDuration;
    std::size_t frames_read = 0;
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
            const auto error = errno_detail("Waiting for selected Ethernet frames");
            close(socket_fd);
            return make_result(model::EthernetTestStatus::Error,
                               "Passive listen failed",
                               error);
        }
        if (ready == 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        if (frames_read >= kMaxFramesRead) {
            observations.mark_incomplete();
            break;
        }
        std::array<uint8_t, kMaxFrameBytes> buffer{};
        iovec payload{buffer.data(), buffer.size()};
        msghdr message{};
        message.msg_iov = &payload;
        message.msg_iovlen = 1;
        const auto count = recvmsg(socket_fd, &message, MSG_TRUNC);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            const auto error = errno_detail("Reading selected Ethernet frame");
            close(socket_fd);
            return make_result(model::EthernetTestStatus::Error,
                               "Passive listen failed",
                               error);
        }
        ++frames_read;
        if (static_cast<std::size_t>(count) > buffer.size()) {
            observations.mark_incomplete();
            continue;
        }
        const std::vector<uint8_t> frame(buffer.begin(), buffer.begin() + count);
        const auto parsed = parse_ethernet_frame(frame);
        if (parsed) {
            observations.ingest(*parsed);
        }
    }
    close(socket_fd);

    const auto captured = observations.summary();
    std::ostringstream detail;
    detail << "Interface: eth0\nDuration: up to 5 seconds\n"
           << "Selected frames read: " << frames_read << "\n"
           << "ARP sender observations: " << captured.arp_frames << "\n"
           << "Possible IPv4 conflicts: " << captured.possible_ip_conflicts.size() << "\n"
           << "DHCP offers observed: " << captured.dhcp_offer_frames << "\n"
           << "Distinct DHCP servers: " << captured.dhcp_servers.size() << "\n"
           << "STP BPDUs: " << captured.stp_frames << "\n"
           << "No DHCP request was sent; multiple servers are not automatically rogue.\n"
           << "No matching announcements is not evidence that the network is down.";
    for (const auto& sender : captured.arp_senders) {
        for (const auto& mac : sender.mac_addresses) {
            detail << "\nARP: " << sender.address << " / " << mac;
        }
    }
    append_list(detail, captured.possible_ip_conflicts);
    for (const auto& server : captured.dhcp_servers) {
        detail << "\nDHCP server: " << server;
    }
    append_list(detail, captured.stp_bpdus);
    if (captured.incomplete) {
        detail << "\nObservation incomplete: a frame or memory budget was reached.";
    }
    if (cancelled) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Passive listen cancelled",
                           detail.str());
    }
    return make_result(model::EthernetTestStatus::Passed,
                       captured.incomplete ? "Passive listen partial" : "Passive listen complete",
                       detail.str());
#else
    (void)cancel_requested;
    return make_result(model::EthernetTestStatus::Unsupported,
                       "Passive listen unavailable",
                       "ARP/DHCP/STP listening is supported only on Linux.");
#endif
}

} // namespace platform::field

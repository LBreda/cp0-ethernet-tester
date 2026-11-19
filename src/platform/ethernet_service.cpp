/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

// Background execution of Ethernet test categories and helpers for external tools.

#include "ethernet_service.h"
#include "ethernet_diagnostics.h"
#include "ethernet_field.h"
#include "ethernet_mdns.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

constexpr const char* kInterfaceName = "eth0";
constexpr auto kGatewayPingTimeout = std::chrono::milliseconds(3200);
constexpr auto kDiscoveryScanTimeout = std::chrono::seconds(12);
constexpr std::size_t kMaxDiscoveryOutputBytes = 64 * 1024;

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
#endif

model::EthernetTestResult make_result(model::EthernetTestStatus status,
                                      std::string summary,
                                      std::string detail) {
    return {status, std::move(summary), std::move(detail)};
}

std::string read_value(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    input >> value;
    return value;
}

bool interface_exists() {
    return std::filesystem::exists(std::filesystem::path("/sys/class/net") / kInterfaceName);
}

std::string ipv4_address() {
#if defined(__linux__)
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) {
        return {};
    }

    std::string result;
    for (auto* address = addresses; address; address = address->ifa_next) {
        if (!address->ifa_addr || address->ifa_addr->sa_family != AF_INET ||
            address->ifa_name != std::string(kInterfaceName)) {
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
#else
    return {};
#endif
}

std::string default_gateway() {
#if defined(__linux__)
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

        unsigned long parsed_flags = 0;
        unsigned long parsed_gateway = 0;
        try {
            parsed_flags = std::stoul(flags, nullptr, 16);
            parsed_gateway = std::stoul(gateway, nullptr, 16);
        } catch (...) {
            continue;
        }
        if ((parsed_flags & 0x2UL) == 0 || parsed_gateway == 0) {
            continue;
        }

        in_addr address{};
        address.s_addr = static_cast<in_addr_t>(parsed_gateway);
        char buffer[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &address, buffer, sizeof(buffer))) {
            return buffer;
        }
    }
#endif
    return {};
}

model::EthernetTestResult run_counter_snapshot() {
    struct CounterField {
        const char* label;
        const char* file;
    };
    constexpr std::array<CounterField, 6> counters = {{
        {"TX bytes", "tx_bytes"},
        {"RX bytes", "rx_bytes"},
        {"TX packets", "tx_packets"},
        {"RX packets", "rx_packets"},
        {"TX errors", "tx_errors"},
        {"RX errors", "rx_errors"},
    }};

    std::ostringstream detail;
    detail << "Cumulative eth0 counters (not a speed measurement):\n";
    bool counter_available = false;
    for (const auto& counter : counters) {
        std::uint64_t value = 0;
        std::ifstream input(std::filesystem::path("/sys/class/net/eth0/statistics") / counter.file);
        if (input >> value) {
            detail << counter.label << ": " << value << '\n';
            counter_available = true;
        } else {
            detail << counter.label << ": unavailable\n";
        }
    }
    detail << "Snapshot only; no traffic was generated.";

    return make_result(counter_available ? model::EthernetTestStatus::Passed
                                         : model::EthernetTestStatus::Unsupported,
                       counter_available ? "Interface counters read" : "Interface counters unavailable",
                       detail.str());
}

bool is_valid_mac_address(const std::string& address) {
    if (address.size() != 17) {
        return false;
    }
    for (std::size_t index = 0; index < address.size(); ++index) {
        if (index % 3 == 2) {
            if (address[index] != ':') {
                return false;
            }
        } else if (!((address[index] >= '0' && address[index] <= '9') ||
                     (address[index] >= 'a' && address[index] <= 'f') ||
                     (address[index] >= 'A' && address[index] <= 'F'))) {
            return false;
        }
    }
    return true;
}

model::EthernetTestResult run_discovery_scan(const std::atomic<bool>& cancel_requested) {
#if defined(__linux__)
    if (cancel_requested.load()) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Scan cancelled",
                           "No ARP discovery packets were sent.");
    }

    const char* executable = nullptr;
    constexpr std::array<const char*, 4> executable_paths = {
        "/usr/sbin/arp-scan", "/usr/bin/arp-scan", "/sbin/arp-scan", "/bin/arp-scan"};
    for (const auto* path : executable_paths) {
        if (access(path, X_OK) == 0) {
            executable = path;
            break;
        }
    }
    if (!executable) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "arp-scan is not installed",
                           "Install arp-scan to discover IPv4 peers on eth0's local subnet.");
    }

    const auto arguments = EthernetService::discovery_scan_arguments();
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>("arp-scan"));
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    int pipe_fds[2]{};
    if (pipe(pipe_fds) != 0) {
        return make_result(model::EthernetTestStatus::Error,
                           "Could not start discovery",
                           "The tester could not create an output pipe for arp-scan.");
    }

    const pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return make_result(model::EthernetTestStatus::Error,
                           "Could not start discovery",
                           "The tester could not create the bounded arp-scan process.");
    }
    if (child == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        execv(executable, argv.data());
        _exit(127);
    }

    close(pipe_fds[1]);
    const int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        int wait_status = 0;
        terminate_and_reap(child, wait_status);
        close(pipe_fds[0]);
        return make_result(model::EthernetTestStatus::Error,
                           "Could not read discovery output",
                           "The tester could not configure a non-blocking output pipe.");
    }

    std::string output;
    output.reserve(4096);
    bool child_exited = false;
    bool output_exceeded = false;
    bool timed_out = false;
    int wait_status = 0;
    const auto started_at = std::chrono::steady_clock::now();
    while (!child_exited) {
        char buffer[2048];
        for (;;) {
            const auto count = read(pipe_fds[0], buffer, sizeof(buffer));
            if (count > 0) {
                const auto bytes = static_cast<std::size_t>(count);
                if (output.size() + bytes > kMaxDiscoveryOutputBytes) {
                    output_exceeded = true;
                    break;
                }
                output.append(buffer, bytes);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        if (output_exceeded) {
            break;
        }

        const auto waited = waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            child_exited = true;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            terminate_and_reap(child, wait_status);
            close(pipe_fds[0]);
            return make_result(model::EthernetTestStatus::Error,
                               "Could not read discovery result",
                               "The arp-scan process ended unexpectedly.");
        }
        if (cancel_requested.load()) {
            break;
        }
        if (std::chrono::steady_clock::now() - started_at >= kDiscoveryScanTimeout) {
            timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!child_exited) {
        terminate_and_reap(child, wait_status);
    }
    char remaining[2048];
    for (;;) {
        const auto count = read(pipe_fds[0], remaining, sizeof(remaining));
        if (count > 0) {
            const auto bytes = static_cast<std::size_t>(count);
            if (output.size() + bytes > kMaxDiscoveryOutputBytes) {
                output_exceeded = true;
                break;
            }
            output.append(remaining, bytes);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(pipe_fds[0]);

    if (output_exceeded) {
        return make_result(model::EthernetTestStatus::Error,
                           "Discovery output limit exceeded",
                           "The scan output exceeded 64 KiB. Results were discarded so an incomplete list cannot appear complete.");
    }
    if (cancel_requested.load()) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Scan cancelled",
                           "The eth0 ARP discovery process was stopped; its partial results were discarded.");
    }
    if (timed_out) {
        return make_result(model::EthernetTestStatus::Failed,
                           "Discovery timed out",
                           "The eth0 scan exceeded its 12-second limit. Partial results were discarded.");
    }
    if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 127) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "arp-scan is not installed",
                           "Install arp-scan to discover IPv4 peers on eth0's local subnet.");
    }
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        std::string detail = output.empty()
                                 ? "arp-scan failed. It may require root or CAP_NET_RAW; no privilege elevation is performed."
                                 : output;
        return make_result(model::EthernetTestStatus::Failed,
                           "ARP discovery failed",
                           std::move(detail));
    }

    const auto peers = EthernetService::parse_discovery_scan_output(output);
    std::string detail = "IPv4 peers found (" + std::to_string(peers.size()) + ")" +
                         "\nOne bounded ARP scan of the local subnet.\n";
    for (const auto& peer : peers) {
        const auto row = peer.first + ": " + peer.second + "\n";
        if (detail.size() + row.size() > EthernetService::kMaxDiscoveryDetailBytes) {
            return make_result(model::EthernetTestStatus::Error,
                               "Discovery result too large",
                               "The complete peer list exceeds the display safety limit; no partial list is shown.");
        }
        detail += row;
    }
    if (peers.empty()) {
        detail += "No IPv4 peers responded to ARP on the local subnet.";
    }
    return make_result(model::EthernetTestStatus::Passed,
                       peers.empty() ? "No peers responded" : "ARP peers discovered",
                       std::move(detail));
#else
    (void)cancel_requested;
    return make_result(model::EthernetTestStatus::Unsupported,
                       "Discovery unavailable",
                       "Local IPv4 ARP discovery is supported only on Linux.");
#endif
}

model::EthernetTestResult run_gateway_ping(const std::atomic<bool>& cancel_requested) {
#if defined(__linux__)
    const auto gateway = default_gateway();
    const auto arguments = EthernetService::gateway_ping_arguments(gateway);
    if (arguments.empty()) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "No eth0 gateway",
                           "Assign a default IPv4 route to eth0 before testing connectivity.");
    }

    if (cancel_requested.load()) {
        return make_result(model::EthernetTestStatus::Cancelled,
                           "Test cancelled",
                           "No gateway probe was sent.");
    }

    if (access("/usr/bin/ping", X_OK) != 0 && access("/bin/ping", X_OK) != 0) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "ping is not installed",
                           "Install iputils-ping to enable the one-packet gateway test.");
    }

    const pid_t child = fork();
    if (child < 0) {
        return make_result(model::EthernetTestStatus::Error,
                           "Could not start ping",
                           "The tester could not create the bounded gateway probe process.");
    }
    if (child == 0) {
        execl("/usr/bin/ping", "ping", arguments[0].c_str(), arguments[1].c_str(),
              arguments[2].c_str(), arguments[3].c_str(), arguments[4].c_str(),
              arguments[5].c_str(), arguments[6].c_str(), arguments[7].c_str(), nullptr);
        execl("/bin/ping", "ping", arguments[0].c_str(), arguments[1].c_str(),
              arguments[2].c_str(), arguments[3].c_str(), arguments[4].c_str(),
              arguments[5].c_str(), arguments[6].c_str(), arguments[7].c_str(), nullptr);
        _exit(127);
    }

    const auto started_at = std::chrono::steady_clock::now();
    int wait_status = 0;
    while (std::chrono::steady_clock::now() - started_at < kGatewayPingTimeout) {
        const auto waited = waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
                return make_result(model::EthernetTestStatus::Passed,
                                   "Gateway replied",
                                   "One ICMP echo was sent to " + gateway + " on eth0.");
            }
            return make_result(model::EthernetTestStatus::Failed,
                               "Gateway did not reply",
                               "One ICMP echo to " + gateway + " failed or ping is unavailable.");
        }
        if (waited < 0) {
            return make_result(model::EthernetTestStatus::Error,
                               "Could not read ping result",
                               "The gateway probe process ended unexpectedly.");
        }
        if (cancel_requested.load()) {
            kill(child, SIGTERM);
            waitpid(child, &wait_status, 0);
            return make_result(model::EthernetTestStatus::Cancelled,
                               "Test cancelled",
                               "The gateway probe was stopped.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    kill(child, SIGTERM);
    waitpid(child, &wait_status, 0);
    return make_result(model::EthernetTestStatus::Failed,
                       "Gateway probe timed out",
                       "The single gateway probe exceeded its 3.2 second safety limit.");
#else
    (void)cancel_requested;
    return make_result(model::EthernetTestStatus::Unsupported,
                       "Connectivity test unavailable",
                       "Gateway ping is supported only on Linux.");
#endif
}

model::EthernetTestResult run_local_test(model::EthernetCategory category,
                                         const std::atomic<bool>& cancel_requested) {
    if (!interface_exists()) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "eth0 not found",
                           "The Linux backend checks eth0; no simulated hardware result is shown.");
    }

    switch (category) {
        case model::EthernetCategory::Link:
            return diagnostics::monitor_link(cancel_requested);
        case model::EthernetCategory::Configuration: {
            const auto address = ipv4_address();
            const auto mac = read_value("/sys/class/net/eth0/address");
            const auto gateway = default_gateway();
            if (address.empty()) {
                return make_result(model::EthernetTestStatus::Failed,
                                   "No IPv4 address on eth0",
                                   "IPv4: not assigned\nInterface configuration is read-only.");
            }
            std::string detail = "Interface: eth0\nIPv4: " + address;
            detail += "\nMAC: " + (mac.empty() ? std::string("unavailable") : mac);
            detail += "\nGateway: " + (gateway.empty() ? std::string("none") : gateway);
            return make_result(model::EthernetTestStatus::Passed, "eth0 is configured", std::move(detail));
        }
        case model::EthernetCategory::Advanced: {
            std::error_code driver_error;
            const auto driver = std::filesystem::read_symlink(
                "/sys/class/net/eth0/device/driver", driver_error);
            const auto duplex = read_value("/sys/class/net/eth0/duplex");
            const auto speed = read_value("/sys/class/net/eth0/speed");
            std::string detail = "Interface: eth0\nDriver: ";
            detail += driver.empty() ? "unavailable" : driver.filename().string();
            detail += "\nDuplex: " + (duplex.empty() ? std::string("unavailable") : duplex);
            detail += "\nSpeed: " + (speed.empty() || speed == "-1"
                                           ? std::string("unavailable")
                                           : speed + " Mb/s");
            if (driver.empty() && duplex.empty() && (speed.empty() || speed == "-1")) {
                return make_result(model::EthernetTestStatus::Unsupported,
                                   "Driver diagnostics unavailable",
                                   detail + "\nNo driver fields are exposed; cable diagnostics are unsupported.");
            }
            return make_result(model::EthernetTestStatus::Passed,
                               "Driver diagnostics read",
                               detail + "\nCable diagnostics are not supported.");
        }
        case model::EthernetCategory::Performance:
            return run_counter_snapshot();
        case model::EthernetCategory::Discovery:
            return run_discovery_scan(cancel_requested);
        case model::EthernetCategory::Connectivity:
            break;
        case model::EthernetCategory::Neighbors:
            return diagnostics::listen_for_neighbors(cancel_requested);
        case model::EthernetCategory::Dns:
            return diagnostics::lookup_gateway_ptr(cancel_requested);
        case model::EthernetCategory::Mdns:
            return mdns::browse_mdns(cancel_requested);
        case model::EthernetCategory::Dhcp:
            return EthernetService::read_dhcp_lease();
        case model::EthernetCategory::Listen:
            return EthernetService::listen_for_field_events(cancel_requested);
        case model::EthernetCategory::Summary:
            break;
    }
    return make_result(model::EthernetTestStatus::Unsupported,
                       "Diagnostic unavailable",
                       "No read-only diagnostic is registered for this category.");
}

} // namespace

EthernetService::EthernetService(Runner runner)
    : runner_(std::move(runner)) {}

EthernetService::~EthernetService() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool EthernetService::start(model::EthernetCategory category) {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result_.status == model::EthernetTestStatus::Running) {
                return false;
            }
        }
        worker_.join();
    }

    cancel_requested_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = make_result(model::EthernetTestStatus::Running,
                              "Test running",
                              "Checking eth0 without changing its configuration.");
    }

    worker_ = std::thread([this, category] {
        model::EthernetTestResult completed;
        try {
            if (runner_) {
                completed = runner_(category, cancel_requested_);
            } else if (category == model::EthernetCategory::Connectivity) {
                completed = run_gateway_ping(cancel_requested_);
            } else {
                completed = run_local_test(category, cancel_requested_);
            }
        } catch (const std::exception& error) {
            completed = make_result(model::EthernetTestStatus::Error,
                                    "Ethernet test failed",
                                    error.what());
        } catch (...) {
            completed = make_result(model::EthernetTestStatus::Error,
                                    "Ethernet test failed",
                                    "An unexpected error occurred.");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(completed);
    });
    return true;
}

void EthernetService::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result_.status == model::EthernetTestStatus::Running) {
        cancel_requested_.store(true);
    }
}

model::EthernetTestResult EthernetService::result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

model::EthernetTestResult EthernetService::read_dhcp_lease() {
    const auto append_lease_details = [](const field::DhcpLease& lease, const char* source) {
        std::ostringstream detail;
        detail << "Interface: eth0\nLease source: " << source << "\nIPv4 address: " << lease.address
               << "\nGateway: " << (lease.router.empty() ? "not present in lease" : lease.router)
               << "\nDHCP server: "
               << (lease.server_address.empty() ? "not present in lease" : lease.server_address);
        if (lease.dns_servers.empty()) {
            detail << "\nDNS servers: not present in lease";
        } else {
            for (const auto& resolver : lease.dns_servers) {
                detail << "\nDNS server: " << resolver;
            }
        }
        detail << "\nLease lifetime: ";
        detail << (lease.lifetime_seconds ? std::to_string(*lease.lifetime_seconds) + " s"
                                          : "not present");
        detail << "\nRenewal time (T1): ";
        detail << (lease.renewal_seconds ? std::to_string(*lease.renewal_seconds) + " s"
                                         : "not present");
        detail << "\nRebinding time (T2): ";
        detail << (lease.rebinding_seconds ? std::to_string(*lease.rebinding_seconds) + " s"
                                           : "not present");
        detail << "\nRead-only inspection; no DHCP request or renewal was sent.";
        return make_result(model::EthernetTestStatus::Passed,
                           "Existing DHCP lease read",
                           detail.str());
    };

    const auto networkmanager_lease = field::read_networkmanager_lease("eth0", ipv4_address());
    if (networkmanager_lease) {
        return append_lease_details(*networkmanager_lease, "NetworkManager (nmcli)");
    }

    const auto lease = field::read_systemd_networkd_lease("eth0");
    if (!lease) {
        return make_result(model::EthernetTestStatus::Unsupported,
                           "No readable DHCP lease",
                           "No matching current NetworkManager DHCP lease could be read with nmcli (which may be unavailable), and no valid systemd-networkd lease was found for eth0. Other network managers are not parsed; an IPv4 address alone does not prove DHCP was used.");
    }
    return append_lease_details(*lease, "systemd-networkd");
}

model::EthernetTestResult EthernetService::listen_for_field_events(
    const std::atomic<bool>& cancel_requested) {
    return field::listen_for_field_events(cancel_requested);
}

bool EthernetService::is_valid_ipv4_address(const std::string& address) {
    std::size_t start = 0;
    for (std::size_t part = 0; part < 4; ++part) {
        const auto end = address.find('.', start);
        const auto length = (end == std::string::npos ? address.size() : end) - start;
        if (length == 0 || length > 3 || (length > 1 && address[start] == '0')) {
            return false;
        }

        unsigned value = 0;
        for (std::size_t index = start; index < start + length; ++index) {
            const char digit = address[index];
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

std::vector<std::string> EthernetService::gateway_ping_arguments(const std::string& gateway) {
    if (!is_valid_ipv4_address(gateway)) {
        return {};
    }
    return {"-n", "-c", "1", "-W", "2", "-I", kInterfaceName, gateway};
}

std::vector<std::string> EthernetService::discovery_scan_arguments() {
    return {"--interface=eth0", "--localnet", "--retry=1", "--timeout=500", "--bandwidth=100000"};
}

std::vector<std::pair<std::string, std::string>> EthernetService::parse_discovery_scan_output(
    const std::string& output) {
    std::vector<std::pair<std::string, std::string>> peers;
    std::set<std::string> seen_addresses;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream row(line);
        std::string address;
        std::string mac;
        if (!(row >> address >> mac) || !is_valid_ipv4_address(address) ||
            !is_valid_mac_address(mac) || !seen_addresses.insert(address).second) {
            continue;
        }
        peers.emplace_back(std::move(address), std::move(mac));
    }
    return peers;
}

} // namespace platform
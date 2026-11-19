/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

// mDNS/DNS-SD packet parsing, record caching and service browsing with strict resource limits.

#include "ethernet_mdns.h"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#if defined(__linux__)
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace platform::mdns {
namespace {

constexpr uint16_t kDnsClassIn = 1;
constexpr uint16_t kDnsTypeA = static_cast<uint16_t>(RecordType::A);
constexpr uint16_t kDnsTypePtr = static_cast<uint16_t>(RecordType::Ptr);
constexpr uint16_t kDnsTypeTxt = static_cast<uint16_t>(RecordType::Txt);
constexpr uint16_t kDnsTypeAaaa = static_cast<uint16_t>(RecordType::Aaaa);
constexpr uint16_t kDnsTypeSrv = static_cast<uint16_t>(RecordType::Srv);
constexpr std::size_t kMaxNamePointerHops = 128;
constexpr auto kCacheFlushGrace = std::chrono::seconds(1);

uint16_t read_u16(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8U) |
                                 static_cast<uint16_t>(bytes[offset + 1]));
}

uint32_t read_u32(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return (static_cast<uint32_t>(bytes[offset]) << 24U) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8U) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

std::string safe_label(const uint8_t* bytes, std::size_t length) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (std::size_t index = 0; index < length; ++index) {
        const auto value = bytes[index];
        if (value >= 0x20U && value <= 0x7eU && value != '.' && value != '\\') {
            result.push_back(static_cast<char>(value));
        } else if (value == '.' || value == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(value));
        } else {
            result += "\\x";
            result.push_back(hex[value >> 4U]);
            result.push_back(hex[value & 0x0fU]);
        }
    }
    return result;
}

std::string safe_txt(const uint8_t* bytes, std::size_t length) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        const auto value = bytes[index];
        if (value >= 0x20U && value <= 0x7eU && value != '\\') {
            result.push_back(static_cast<char>(value));
        } else if (value == '\\') {
            result += "\\\\";
        } else {
            result += "\\x";
            result.push_back(hex[value >> 4U]);
            result.push_back(hex[value & 0x0fU]);
        }
    }
    return result;
}

bool read_name(const std::vector<uint8_t>& packet,
               std::size_t& offset,
               std::size_t encoded_end,
               std::string& name) {
    std::size_t cursor = offset;
    std::size_t consumed = 0;
    std::size_t expanded_length = 1;
    bool jumped = false;
    bool terminated = false;
    std::vector<std::size_t> visited_pointers;
    std::vector<std::string> labels;

    for (std::size_t steps = 0; steps < kMaxNamePointerHops; ++steps) {
        if (cursor >= packet.size() || (!jumped && cursor >= encoded_end)) {
            return false;
        }
        const auto length = packet[cursor];
        // Compression pointer: only backward jumps without loops are accepted.
        if ((length & 0xc0U) == 0xc0U) {
            if (cursor + 1 >= packet.size() || (!jumped && cursor + 2 > encoded_end)) {
                return false;
            }
            const auto target = static_cast<std::size_t>(
                ((static_cast<uint16_t>(length & 0x3fU) << 8U) | packet[cursor + 1]));
            if (target >= cursor || target >= packet.size() ||
                std::find(visited_pointers.begin(), visited_pointers.end(), target) !=
                    visited_pointers.end()) {
                return false;
            }
            visited_pointers.push_back(target);
            if (!jumped) {
                consumed += 2;
            }
            cursor = target;
            jumped = true;
            continue;
        }
        if ((length & 0xc0U) != 0) {
            return false;
        }
        ++cursor;
        if (!jumped) {
            ++consumed;
        }
        if (length == 0) {
            terminated = true;
            break;
        }
        const auto label_length = static_cast<std::size_t>(length);
        if (label_length > packet.size() - cursor ||
            (!jumped && label_length > encoded_end - cursor) ||
            expanded_length + label_length + 1 > 255) {
            return false;
        }
        labels.push_back(safe_label(packet.data() + cursor, label_length));
        expanded_length += label_length + 1;
        cursor += label_length;
        if (!jumped) {
            consumed += label_length;
        }
    }

    if (!terminated || consumed == 0) {
        return false;
    }
    name.clear();
    for (const auto& label : labels) {
        if (!name.empty()) {
            name.push_back('.');
        }
        name += label;
    }
    if (labels.empty()) {
        name = ".";
    }
    if (name.size() > 1024) {
        return false;
    }
    offset += consumed;
    return true;
}

std::string lower_ascii(std::string value) {
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 'A' && byte <= 'Z') {
            character = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return value;
}

bool same_name(const std::string& left, const std::string& right) {
    return lower_ascii(left) == lower_ascii(right);
}

bool is_service_type(const std::string& name) {
    const auto lowered = lower_ascii(name);
    const auto suffix_start = lowered.rfind("._tcp.local");
    const auto udp_suffix_start = lowered.rfind("._udp.local");
    const auto marker = suffix_start != std::string::npos ? suffix_start : udp_suffix_start;
    if (marker == std::string::npos || marker == 0 || lowered.find('.', 0) != marker) {
        return false;
    }
    return lowered.front() == '_';
}

std::string service_type_display(const std::string& fqdn) {
    constexpr std::size_t local_suffix_length = 6;
    return fqdn.size() > local_suffix_length
               ? fqdn.substr(0, fqdn.size() - local_suffix_length)
               : fqdn;
}

bool supported_type(uint16_t type) {
    return type == kDnsTypeA || type == kDnsTypePtr || type == kDnsTypeTxt ||
           type == kDnsTypeAaaa || type == kDnsTypeSrv;
}

bool same_rdata(const ResourceRecord& left, const ResourceRecord& right) {
    return same_name(left.owner, right.owner) && left.type == right.type &&
           same_name(left.target, right.target) && left.priority == right.priority &&
           left.weight == right.weight && left.port == right.port &&
           left.text == right.text && left.address == right.address;
}

std::size_t record_bytes(const ResourceRecord& record) {
    std::size_t size = sizeof(ResourceRecord) + record.owner.size() + record.target.size() +
                       record.address.size();
    for (const auto& value : record.text) {
        size += value.size();
    }
    return size;
}

bool is_enumeration_owner(const std::string& name) {
    return same_name(name, "_services._dns-sd._udp.local");
}

} // namespace

ParsedPacket parse_packet(const std::vector<uint8_t>& packet) {
    ParsedPacket parsed;
    const auto fail = [&parsed](const char* reason) {
        parsed.valid = false;
        parsed.incomplete = true;
        parsed.error = reason;
        parsed.records.clear();
    };

    if (packet.size() < 12) {
        fail("DNS header is truncated");
        return parsed;
    }
    if (packet.size() > kMaxPacketBytes) {
        fail("Packet exceeds the parser byte limit");
        return parsed;
    }

    const auto flags = read_u16(packet, 2);
    parsed.truncated = (flags & 0x0200U) != 0;
    parsed.incomplete = parsed.truncated;
    const auto question_count = static_cast<std::size_t>(read_u16(packet, 4));
    const auto answer_count = static_cast<std::size_t>(read_u16(packet, 6));
    const auto authority_count = static_cast<std::size_t>(read_u16(packet, 8));
    const auto additional_count = static_cast<std::size_t>(read_u16(packet, 10));
    std::size_t offset = 12;

    if (question_count > kMaxQuestionsPerPacket) {
        parsed.valid = true;
        parsed.incomplete = true;
        parsed.error = "Question count exceeds the parser limit";
        return parsed;
    }
    for (std::size_t index = 0; index < question_count; ++index) {
        std::string question_name;
        if (!read_name(packet, offset, packet.size(), question_name) ||
            packet.size() - offset < 4) {
            fail("Question section is malformed");
            return parsed;
        }
        offset += 4;
    }

    const auto total_records = answer_count + authority_count + additional_count;
    if (total_records > kMaxRecordsPerPacket) {
        parsed.valid = true;
        parsed.incomplete = true;
        parsed.error = "Resource record count exceeds the parser limit";
        return parsed;
    }

    for (std::size_t index = 0; index < total_records; ++index) {
        ResourceRecord record;
        if (!read_name(packet, offset, packet.size(), record.owner) ||
            packet.size() - offset < 10) {
            fail("Resource record header is malformed");
            return parsed;
        }
        const auto wire_type = read_u16(packet, offset);
        const auto wire_class = read_u16(packet, offset + 2);
        record.ttl = read_u32(packet, offset + 4);
        const auto data_length = static_cast<std::size_t>(read_u16(packet, offset + 8));
        offset += 10;
        if (data_length > packet.size() - offset) {
            fail("Resource record data is truncated");
            return parsed;
        }
        const auto data_start = offset;
        const auto data_end = data_start + data_length;
        offset = data_end;

        if ((wire_class & 0x7fffU) != kDnsClassIn || !supported_type(wire_type)) {
            continue;
        }
        record.type = static_cast<RecordType>(wire_type);
        record.cache_flush = (wire_class & 0x8000U) != 0;
        std::size_t data_cursor = data_start;
        switch (wire_type) {
            case kDnsTypePtr:
                if (!read_name(packet, data_cursor, data_end, record.target) || data_cursor != data_end) {
                    fail("PTR record data is malformed");
                    return parsed;
                }
                break;
            case kDnsTypeSrv:
                if (data_length < 7) {
                    fail("SRV record data is truncated");
                    return parsed;
                }
                record.priority = read_u16(packet, data_cursor);
                record.weight = read_u16(packet, data_cursor + 2);
                record.port = read_u16(packet, data_cursor + 4);
                data_cursor += 6;
                if (!read_name(packet, data_cursor, data_end, record.target) || data_cursor != data_end) {
                    fail("SRV target name is malformed");
                    return parsed;
                }
                break;
            case kDnsTypeTxt: {
                std::size_t txt_bytes = 0;
                while (data_cursor < data_end) {
                    const auto length = static_cast<std::size_t>(packet[data_cursor++]);
                    if (length > data_end - data_cursor) {
                        fail("TXT character string is truncated");
                        return parsed;
                    }
                    if (record.text.size() < kMaxTxtEntries && txt_bytes + length <= kMaxTxtBytes) {
                        record.text.push_back(safe_txt(packet.data() + data_cursor, length));
                        txt_bytes += length;
                    } else {
                        parsed.incomplete = true;
                        parsed.error = "TXT metadata exceeds the parser limit";
                    }
                    data_cursor += length;
                }
                break;
            }
            case kDnsTypeA:
            case kDnsTypeAaaa: {
                const auto expected_length = wire_type == kDnsTypeA ? 4U : 16U;
                if (data_length != expected_length) {
                    fail("Address record has an invalid length");
                    return parsed;
                }
                std::array<char, INET6_ADDRSTRLEN> address{};
                const auto family = wire_type == kDnsTypeA ? AF_INET : AF_INET6;
                if (inet_ntop(family, packet.data() + data_start, address.data(), address.size()) == nullptr) {
                    fail("Address record could not be formatted");
                    return parsed;
                }
                record.address = address.data();
                break;
            }
            default:
                break;
        }
        parsed.records.push_back(std::move(record));
    }

    parsed.valid = true;
    return parsed;
}

void ServiceCache::ingest(const ParsedPacket& packet, Clock::time_point now) {
    if (!packet.valid) {
        return;
    }
    incomplete_ = incomplete_ || packet.incomplete || packet.truncated;
    expire(now);

    for (const auto& record : packet.records) {
        if (record.ttl == 0) {
            for (auto iterator = records_.begin(); iterator != records_.end();) {
                if (same_rdata(iterator->record, record)) {
                    cache_bytes_ -= iterator->bytes;
                    iterator = records_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
            continue;
        }

        if (record.cache_flush) {
            for (auto iterator = records_.begin(); iterator != records_.end();) {
                const bool same_record_set = same_name(iterator->record.owner, record.owner) &&
                                             iterator->record.type == record.type;
                const bool recently_received = now - iterator->received_at <= kCacheFlushGrace;
                if (same_record_set && !same_rdata(iterator->record, record) && !recently_received) {
                    cache_bytes_ -= iterator->bytes;
                    iterator = records_.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }

        const auto existing = std::find_if(records_.begin(), records_.end(), [&record](const Entry& entry) {
            return same_rdata(entry.record, record);
        });
        const auto ttl = std::min(record.ttl, kMaxRecordTtlSeconds);
        const auto expiration = now + std::chrono::seconds(ttl);
        if (existing != records_.end()) {
            existing->record.ttl = ttl;
            existing->record.cache_flush = record.cache_flush;
            existing->received_at = now;
            existing->expires_at = expiration;
            continue;
        }

        const auto bytes = record_bytes(record);
        if (records_.size() >= kMaxCachedRecords || bytes > kMaxCacheBytes - cache_bytes_) {
            incomplete_ = true;
            continue;
        }
        records_.push_back(Entry{record, now, expiration, bytes});
        cache_bytes_ += bytes;
    }
}

void ServiceCache::expire(Clock::time_point now) {
    for (auto iterator = records_.begin(); iterator != records_.end();) {
        if (iterator->expires_at <= now) {
            cache_bytes_ -= iterator->bytes;
            iterator = records_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

std::vector<std::string> ServiceCache::service_types(Clock::time_point now) {
    expire(now);
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& entry : records_) {
        const auto& record = entry.record;
        if (record.type != RecordType::Ptr || !is_enumeration_owner(record.owner) ||
            !is_service_type(record.target)) {
            continue;
        }
        const auto key = lower_ascii(record.target);
        if (seen.insert(key).second) {
            if (result.size() == kMaxServiceInstances) {
                incomplete_ = true;
                continue;
            }
            result.push_back(service_type_display(record.target));
        }
    }
    std::sort(result.begin(), result.end(), [](const std::string& left, const std::string& right) {
        return lower_ascii(left) < lower_ascii(right);
    });
    return result;
}

std::vector<ServiceInstance> ServiceCache::services(Clock::time_point now) {
    expire(now);
    std::vector<ServiceInstance> result;
    std::set<std::string> seen;

    for (const auto& ptr_entry : records_) {
        const auto& ptr = ptr_entry.record;
        if (ptr.type != RecordType::Ptr || !is_service_type(ptr.owner)) {
            continue;
        }
        const auto suffix = "." + ptr.owner;
        if (ptr.target.size() <= suffix.size() ||
            lower_ascii(ptr.target.substr(ptr.target.size() - suffix.size())) != lower_ascii(suffix)) {
            continue;
        }
        const auto instance_key = lower_ascii(ptr.target);
        if (!seen.insert(instance_key).second) {
            continue;
        }
        if (result.size() == kMaxServiceInstances) {
            incomplete_ = true;
            continue;
        }

        ServiceInstance service;
        service.service_type = service_type_display(ptr.owner);
        service.instance = ptr.target.substr(0, ptr.target.size() - suffix.size());
        service.instance_fqdn = ptr.target;
        const Entry* selected_srv = nullptr;
        for (const auto& entry : records_) {
            const auto& record = entry.record;
            if (record.type == RecordType::Srv && same_name(record.owner, ptr.target) &&
                (selected_srv == nullptr || entry.received_at > selected_srv->received_at)) {
                selected_srv = &entry;
            } else if (record.type == RecordType::Txt && same_name(record.owner, ptr.target)) {
                for (const auto& value : record.text) {
                    if (service.text.size() == kMaxTxtEntries) {
                        incomplete_ = true;
                        break;
                    }
                    if (std::find(service.text.begin(), service.text.end(), value) == service.text.end()) {
                        service.text.push_back(value);
                    }
                }
            }
        }
        if (selected_srv != nullptr && selected_srv->record.target != ".") {
            service.hostname = selected_srv->record.target;
            service.port = selected_srv->record.port;
            for (const auto& entry : records_) {
                const auto& record = entry.record;
                if ((record.type == RecordType::A || record.type == RecordType::Aaaa) &&
                    same_name(record.owner, service.hostname) &&
                    std::find(service.addresses.begin(), service.addresses.end(), record.address) ==
                        service.addresses.end()) {
                    service.addresses.push_back(record.address);
                }
            }
            std::sort(service.addresses.begin(), service.addresses.end());
        }
        result.push_back(std::move(service));
    }

    std::sort(result.begin(), result.end(), [](const ServiceInstance& left, const ServiceInstance& right) {
        const auto left_type = lower_ascii(left.service_type);
        const auto right_type = lower_ascii(right.service_type);
        if (left_type != right_type) {
            return left_type < right_type;
        }
        return lower_ascii(left.instance) < lower_ascii(right.instance);
    });
    return result;
}

bool ServiceCache::incomplete() const {
    return incomplete_;
}

std::size_t ServiceCache::record_count() const {
    return records_.size();
}

std::size_t ServiceCache::cache_bytes() const {
    return cache_bytes_;
}

namespace {

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool encode_dns_name(const std::string& name, std::vector<uint8_t>& wire) {
    std::vector<std::vector<uint8_t>> labels;
    std::vector<uint8_t> label;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto value = name[index];
        if (value == '.') {
            if (label.empty()) {
                if (index + 1 == name.size() && !labels.empty()) break;
                return false;
            }
            labels.push_back(std::move(label));
            label.clear();
            continue;
        }
        if (value == '\\') {
            if (++index >= name.size()) return false;
            if (name[index] == 'x' && index + 2 < name.size()) {
                const auto high = hex_value(name[index + 1]);
                const auto low = hex_value(name[index + 2]);
                if (high < 0 || low < 0) return false;
                label.push_back(static_cast<uint8_t>((high << 4) | low));
                index += 2;
            } else {
                label.push_back(static_cast<uint8_t>(name[index]));
            }
        } else {
            label.push_back(static_cast<uint8_t>(value));
        }
        if (label.empty() || label.size() > 63) return false;
    }
    if (!label.empty()) labels.push_back(std::move(label));
    if (labels.empty()) return false;

    std::size_t wire_length = 1;
    for (const auto& part : labels) wire_length += part.size() + 1;
    if (wire_length > 255) return false;
    for (const auto& part : labels) {
        wire.push_back(static_cast<uint8_t>(part.size()));
        wire.insert(wire.end(), part.begin(), part.end());
    }
    wire.push_back(0);
    return true;
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
    bytes.push_back(static_cast<uint8_t>(value & 0xffU));
}

std::string socket_error(const char* operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

std::string query_key(const std::string& name, RecordType type) {
    return lower_ascii(name) + "#" + std::to_string(static_cast<uint16_t>(type));
}

std::string seconds_text(std::chrono::steady_clock::duration duration) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(1)
           << std::chrono::duration<double>(duration).count() << " s";
    return output.str();
}

} // namespace

std::vector<uint8_t> make_query_packet(const std::string& name, RecordType type) {
    std::vector<uint8_t> packet(12, 0);
    packet[5] = 1;
    if (!encode_dns_name(name, packet)) return {};
    append_u16(packet, static_cast<uint16_t>(type));
    append_u16(packet, kDnsClassIn);
    if (packet.size() > kMaxPacketBytes) return {};
    return packet;
}

#if defined(__linux__)
namespace {

class SocketBrowseTransport final : public BrowseTransport {
public:
    ~SocketBrowseTransport() override {
        close();
    }

    bool open(std::string& error) override {
        close();
        interface_index_ = if_nametoindex("eth0");
        if (interface_index_ == 0) {
            error = "eth0 is unavailable: " + std::string(std::strerror(errno));
            return false;
        }

        descriptor_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        if (descriptor_ < 0) {
            error = socket_error("Unable to create the mDNS UDP socket");
            return false;
        }

        const int enabled = 1;
        if (!set_option(SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled), error,
                        "Unable to enable mDNS socket sharing") ||
            !set_option(IPPROTO_IP, IP_PKTINFO, &enabled, sizeof(enabled), error,
                        "Unable to select the receiving interface")) {
            close();
            return false;
        }

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(5353);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(descriptor_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            error = socket_error("Unable to bind UDP port 5353 (another mDNS responder may not allow sharing)");
            close();
            return false;
        }

        ip_mreqn membership{};
        if (inet_pton(AF_INET, "224.0.0.251", &membership.imr_multiaddr) != 1) {
            error = "Unable to configure the mDNS multicast address.";
            close();
            return false;
        }
        membership.imr_ifindex = static_cast<int>(interface_index_);
        if (!set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership), error,
                        "Unable to join mDNS multicast on eth0")) {
            close();
            return false;
        }
        if (!set_option(IPPROTO_IP, IP_MULTICAST_IF, &membership, sizeof(membership), error,
                        "Unable to select eth0 for mDNS multicast")) {
            close();
            return false;
        }
        const unsigned char ttl = 255;
        if (!set_option(IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl), error,
                        "Unable to set the mDNS multicast TTL")) {
            close();
            return false;
        }
        return true;
    }

    bool send_query(const std::string& name, RecordType type, std::string& error) override {
        const auto packet = make_query_packet(name, type);
        if (packet.empty()) {
            error = "Unable to encode an mDNS query name.";
            return false;
        }

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(5353);
        if (inet_pton(AF_INET, "224.0.0.251", &destination.sin_addr) != 1) {
            error = "Unable to encode the mDNS multicast destination.";
            return false;
        }

        iovec payload{};
        payload.iov_base = const_cast<uint8_t*>(packet.data());
        payload.iov_len = packet.size();
        std::array<unsigned char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
        msghdr message{};
        message.msg_name = &destination;
        message.msg_namelen = sizeof(destination);
        message.msg_iov = &payload;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = IPPROTO_IP;
        header->cmsg_type = IP_PKTINFO;
        header->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
        auto* information = reinterpret_cast<in_pktinfo*>(CMSG_DATA(header));
        information->ipi_ifindex = static_cast<int>(interface_index_);
        information->ipi_spec_dst.s_addr = htonl(INADDR_ANY);

        const auto sent = sendmsg(descriptor_, &message, 0);
        if (sent < 0 || static_cast<std::size_t>(sent) != packet.size()) {
            error = socket_error("Unable to send an mDNS query on eth0");
            return false;
        }
        return true;
    }

    ReceiveStatus receive(std::chrono::milliseconds timeout,
                          std::vector<uint8_t>& packet,
                          std::string& error) override {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) return ReceiveStatus::Timeout;
            pollfd descriptor{descriptor_, POLLIN, 0};
            const auto wait_ms = static_cast<int>(std::max<int64_t>(1, remaining.count()));
            const auto ready = poll(&descriptor, 1, wait_ms);
            if (ready == 0) return ReceiveStatus::Timeout;
            if (ready < 0) {
                if (errno == EINTR) continue;
                error = socket_error("Unable to wait for mDNS responses");
                return ReceiveStatus::Error;
            }
            if ((descriptor.revents & POLLIN) == 0) {
                error = "The mDNS socket closed while browsing.";
                return ReceiveStatus::Error;
            }

            std::array<uint8_t, kMaxPacketBytes + 1> bytes{};
            alignas(cmsghdr) std::array<unsigned char, CMSG_SPACE(sizeof(in_pktinfo))> control{};
            sockaddr_in source{};
            iovec payload{bytes.data(), bytes.size()};
            msghdr message{};
            message.msg_name = &source;
            message.msg_namelen = sizeof(source);
            message.msg_iov = &payload;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            const auto received = recvmsg(descriptor_, &message, MSG_DONTWAIT);
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                error = socket_error("Unable to receive mDNS responses");
                return ReceiveStatus::Error;
            }

            bool received_on_eth0 = false;
            for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
                    header->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
                    const auto* information = reinterpret_cast<const in_pktinfo*>(CMSG_DATA(header));
                    received_on_eth0 = information->ipi_ifindex == static_cast<int>(interface_index_);
                    break;
                }
            }
            if (!received_on_eth0 || (message.msg_flags & MSG_CTRUNC) != 0) continue;

            const auto copied = std::min<std::size_t>(static_cast<std::size_t>(received), bytes.size());
            packet.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(copied));
            if ((message.msg_flags & MSG_TRUNC) != 0 && packet.size() <= kMaxPacketBytes) {
                packet.resize(kMaxPacketBytes + 1);
            }
            return ReceiveStatus::Packet;
        }
    }

    void close() override {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = -1;
        interface_index_ = 0;
    }

private:
    bool set_option(int level,
                    int option,
                    const void* value,
                    socklen_t length,
                    std::string& error,
                    const char* message) {
        if (setsockopt(descriptor_, level, option, value, length) == 0) return true;
        error = socket_error(message);
        return false;
    }

    int descriptor_{-1};
    unsigned int interface_index_{0};
};

} // namespace
#endif

model::EthernetTestResult browse_mdns(const std::atomic<bool>& cancel_requested,
                                     BrowseTransport& transport,
                                     BrowseClock now,
                                     std::chrono::milliseconds duration) {
    if (!now) now = [] { return ServiceCache::Clock::now(); };
    const auto start = now();
    if (cancel_requested.load()) {
        return {model::EthernetTestStatus::Cancelled,
                "mDNS browse cancelled",
                "The browse was cancelled before opening the eth0 socket."};
    }

    std::string open_error;
    if (!transport.open(open_error)) {
        transport.close();
        return {model::EthernetTestStatus::Error,
                "mDNS browse unavailable",
                "Interface: eth0\n" + open_error};
    }
    struct CloseTransport {
        BrowseTransport& transport;
        ~CloseTransport() { transport.close(); }
    } close_transport{transport};

    ServiceCache cache;
    std::set<std::string> sent_queries;
    std::size_t query_count = 0;
    std::size_t packet_count = 0;
    bool incomplete = false;
    std::string failure;

    const auto issue_query = [&](const std::string& name, RecordType type) {
        const auto key = query_key(name, type);
        if (sent_queries.count(key) != 0) return true;
        if (query_count >= kMaxBrowseQueries) {
            incomplete = true;
            return false;
        }
        std::string error;
        if (!transport.send_query(name, type, error)) {
            failure = std::move(error);
            return false;
        }
        sent_queries.insert(key);
        ++query_count;
        return true;
    };

    if (!issue_query("_services._dns-sd._udp.local", RecordType::Ptr)) {
        return {model::EthernetTestStatus::Error,
                "mDNS browse failed",
                "Interface: eth0\n" + (failure.empty() ? std::string("The query budget was exhausted.") : failure)};
    }

    const auto schedule_queries = [&] {
        for (const auto& service_type : cache.service_types(now())) {
            if (!issue_query(service_type + ".local", RecordType::Ptr) && !failure.empty()) return;
        }
        for (const auto& service : cache.services(now())) {
            if (service.instance_fqdn.empty()) continue;
            if (!service.port && !issue_query(service.instance_fqdn, RecordType::Srv) && !failure.empty()) return;
            if (service.text.empty() && !issue_query(service.instance_fqdn, RecordType::Txt) && !failure.empty()) return;
            if (!service.hostname.empty() && service.addresses.empty()) {
                if (!issue_query(service.hostname, RecordType::A) && !failure.empty()) return;
                if (!issue_query(service.hostname, RecordType::Aaaa) && !failure.empty()) return;
            }
        }
    };

    const auto deadline = start + std::max(duration, std::chrono::milliseconds::zero());
    while (now() < deadline) {
        if (cancel_requested.load()) break;
        cache.expire(now());
        schedule_queries();
        if (!failure.empty()) break;
        if (packet_count >= kMaxBrowsePackets) {
            incomplete = true;
            break;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now());
        if (remaining <= std::chrono::milliseconds::zero()) break;
        const auto wait = std::max(std::chrono::milliseconds(1),
                                   std::min(std::chrono::milliseconds(50), remaining));
        std::vector<uint8_t> packet;
        std::string receive_error;
        const auto received = transport.receive(wait, packet, receive_error);
        if (received == ReceiveStatus::Error) {
            failure = std::move(receive_error);
            incomplete = true;
            break;
        }
        if (received == ReceiveStatus::Packet) {
            ++packet_count;
            const auto parsed = parse_packet(packet);
            if (!parsed.valid) {
                incomplete = true;
            } else {
                incomplete = incomplete || parsed.incomplete || parsed.truncated;
                cache.ingest(parsed, now());
            }
        }
    }

    const auto end = now();
    const bool cancelled = cancel_requested.load();
    auto services = cache.services(end);
    incomplete = incomplete || cache.incomplete();
    bool display_truncated = false;
    std::string detail = "mDNS services found (" + std::to_string(services.size()) + ")\n";
    detail += "Browse duration: " + seconds_text(end - start) + "\n";
    detail += "Interface: eth0\nQueries sent: " + std::to_string(query_count) +
              "\nPackets received: " + std::to_string(packet_count) + "\n";
    const auto append_line = [&](const std::string& line) {
        if (detail.size() + line.size() > kMaxBrowseDetailBytes - 192) {
            display_truncated = true;
            return false;
        }
        detail += line;
        detail.push_back('\n');
        return true;
    };
    if (!failure.empty()) append_line("Browse stopped: " + failure);
    if (services.empty()) {
        append_line("No DNS-SD services were heard during this window. This does not indicate network failure; mDNS only reveals services that announce on this local link.");
    } else {
        for (const auto& service : services) {
            if (!append_line(service.service_type + " — " + service.instance) ||
                !append_line("Instance: " + service.instance) ||
                !append_line("Type: " + service.service_type) ||
                !append_line("Hostname: " + (service.hostname.empty() ? std::string("unresolved") : service.hostname)) ||
                !append_line("Port: " + (service.port ? std::to_string(*service.port) : std::string("unresolved")))) {
                break;
            }
            if (service.addresses.empty()) {
                if (!append_line("Address: unresolved")) break;
            } else {
                for (const auto& address : service.addresses) {
                    if (!append_line("Address: " + address)) break;
                }
                if (display_truncated) break;
            }
            if (service.text.empty()) {
                if (!append_line("TXT: none advertised or unresolved")) break;
            } else {
                for (const auto& text : service.text) {
                    if (!append_line("TXT: " + text)) break;
                }
                if (display_truncated) break;
            }
        }
    }
    incomplete = incomplete || display_truncated;
    if (incomplete) {
        detail += "\nINCOMPLETE: A packet, query, cache, parser, or display limit was reached; results may be partial.\n";
    }
    if (!failure.empty()) {
        detail += "\nThe mDNS browse stopped with an explicit transport error.\n";
    }
    if (cancelled) {
        return {model::EthernetTestStatus::Cancelled,
                "mDNS browse cancelled (" + std::to_string(services.size()) + " services)",
                std::move(detail)};
    }
    if (!failure.empty()) {
        return {model::EthernetTestStatus::Error,
                "mDNS browse failed (" + std::to_string(services.size()) + " services)",
                std::move(detail)};
    }
    if (incomplete) {
        return {model::EthernetTestStatus::Failed,
                "mDNS browse incomplete (" + std::to_string(services.size()) + " services)",
                std::move(detail)};
    }
    return {model::EthernetTestStatus::Passed,
            "mDNS browse complete (" + std::to_string(services.size()) + " services)",
            std::move(detail)};
}

model::EthernetTestResult browse_mdns(const std::atomic<bool>& cancel_requested) {
#if defined(__linux__)
    SocketBrowseTransport transport;
    return browse_mdns(cancel_requested,
                       transport,
                       [] { return ServiceCache::Clock::now(); },
                       kBrowseDuration);
#else
    (void)cancel_requested;
    return {model::EthernetTestStatus::Unsupported,
            "mDNS browse unavailable",
            "The integrated mDNS browse requires Linux UDP multicast support."};
#endif
}

} // namespace platform::mdns

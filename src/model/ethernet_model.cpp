/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ethernet_model.cpp
 * @brief EthernetModel storage and health-summary aggregation.
 */

#include "ethernet_model.h"

#include <cctype>
#include <utility>

namespace model {

EthernetCategory EthernetModel::category() const {
    return category_;
}

void EthernetModel::set_category(EthernetCategory category) {
    category_ = category;
}

const EthernetTestResult& EthernetModel::result() const {
    return result(category_);
}

void EthernetModel::set_result(EthernetTestResult result) {
    set_result(category_, std::move(result));
}

const EthernetTestResult& EthernetModel::result(EthernetCategory category) const {
    return results_[static_cast<std::size_t>(category)];
}

void EthernetModel::set_result(EthernetCategory category, EthernetTestResult result) {
    // Terminal results get a completion timestamp so health blocks can order and age them.
    if (result.status != EthernetTestStatus::Ready && result.status != EthernetTestStatus::Running &&
        !result.has_completed_at) {
        result.completed_at = std::chrono::system_clock::now();
        result.has_completed_at = true;
    }
    results_[static_cast<std::size_t>(category)] = std::move(result);
}

EthernetHealthSummary EthernetModel::health_summary() const {
    // Categories contributing to each health block; only the first kCategoryCounts[i]
    // entries of a row are used (the remaining entries are padding).
    constexpr std::array<std::array<EthernetCategory, 4>, 3> kCategories = {{
        {EthernetCategory::Link, EthernetCategory::Advanced, EthernetCategory::Link,
         EthernetCategory::Link},
        {EthernetCategory::Configuration, EthernetCategory::Connectivity, EthernetCategory::Dns,
         EthernetCategory::Dhcp},
        {EthernetCategory::Neighbors, EthernetCategory::Listen, EthernetCategory::Discovery,
         EthernetCategory::Mdns},
    }};
    constexpr std::array<std::size_t, 3> kCategoryCounts = {2, 4, 4};

    EthernetHealthSummary summary;
    for (std::size_t block_index = 0; block_index < summary.blocks.size(); ++block_index) {
        auto& block = summary.blocks[block_index];
        // Worst status wins: any failure is red, any cancelled/unsupported test is yellow.
        bool any_red = false;
        bool any_yellow = false;
        for (std::size_t category_index = 0; category_index < kCategoryCounts[block_index];
             ++category_index) {
            const auto& test = result(kCategories[block_index][category_index]);
            if (!test.has_completed_at) {
                continue;
            }
            if (!block.has_measurement || test.completed_at > block.measured_at) {
                block.measured_at = test.completed_at;
            }
            block.has_measurement = true;
            switch (test.status) {
                case EthernetTestStatus::Failed:
                case EthernetTestStatus::Error:
                    any_red = true;
                    break;
                case EthernetTestStatus::Unsupported:
                case EthernetTestStatus::Cancelled:
                    any_yellow = true;
                    break;
                case EthernetTestStatus::Passed:
                    break;
                case EthernetTestStatus::Ready:
                case EthernetTestStatus::Running:
                    break;
            }
        }
        if (any_red) {
            block.state = EthernetHealthState::Red;
        } else if (any_yellow) {
            block.state = EthernetHealthState::Yellow;
        } else if (block.has_measurement) {
            block.state = EthernetHealthState::Green;
        }
    }

    // If the link test saw a non-zero carrier change count, results of the other blocks
    // measured before that link test may be outdated: downgrade them to yellow and mark stale.
    const auto& link_result = result(EthernetCategory::Link);
    constexpr char kCarrierChanges[] = "Observed carrier changes: ";
    const auto carrier_marker = link_result.detail.find(kCarrierChanges);
    if (carrier_marker != std::string::npos) {
        const auto digit = carrier_marker + sizeof(kCarrierChanges) - 1;
        if (digit < link_result.detail.size() &&
            std::isdigit(static_cast<unsigned char>(link_result.detail[digit])) &&
            link_result.detail[digit] != '0') {
            for (std::size_t block_index = 1; block_index < summary.blocks.size(); ++block_index) {
                auto& block = summary.blocks[block_index];
                if (block.has_measurement && block.measured_at < link_result.completed_at) {
                    block.state = EthernetHealthState::Yellow;
                    block.stale = true;
                }
            }
        }
    }
    return summary;
}

} // namespace model
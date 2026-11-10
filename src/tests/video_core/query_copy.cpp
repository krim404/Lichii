// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/vk_query_copy.h"

namespace {
void CheckCopies(const std::vector<VkBufferCopy>& copies) {
    std::array<unsigned char, 512> source{};
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<unsigned char>((i * 73 + i / 7) & 255);
    }
    std::array<unsigned char, 128> expected{}, actual{};
    for (const auto& copy : copies) {
        std::copy_n(source.begin() + copy.srcOffset, copy.size, expected.begin() + copy.dstOffset);
    }
    const auto normalized = Vulkan::NonOverlappingQueryCopies(copies);
    std::array<bool, 128> written{};
    for (const auto& copy : normalized) {
        REQUIRE(copy.size > 0);
        REQUIRE(copy.size % 4 == 0);
        REQUIRE(copy.srcOffset % 4 == 0);
        REQUIRE(copy.dstOffset % 4 == 0);
        REQUIRE(copy.srcOffset + copy.size <= source.size());
        REQUIRE(copy.dstOffset + copy.size <= actual.size());
        for (size_t i = copy.dstOffset; i < copy.dstOffset + copy.size; ++i) {
            REQUIRE_FALSE(written[i]);
            written[i] = true;
        }
        std::copy_n(source.begin() + copy.srcOffset, copy.size, actual.begin() + copy.dstOffset);
    }
    REQUIRE(actual == expected);
}
} // namespace

TEST_CASE("Query copies preserve ordered writes without destination overlap", "[query_copy]") {
    CheckCopies({});
    CheckCopies({{0, 0, 4}});
    CheckCopies({{0, 4, 4}, {16, 4, 4}});
    CheckCopies({{0, 0, 16}, {32, 4, 4}});
    CheckCopies({{0, 4, 4}, {32, 0, 16}});
    CheckCopies({{0, 0, 8}, {32, 4, 8}, {64, 0, 4}});
    CheckCopies({{0, 0, 4}, {32, 8, 4}, {64, 4, 4}});
    std::mt19937 random{0x04004860};
    for (unsigned trial = 0; trial < 256; ++trial) {
        std::vector<VkBufferCopy> copies;
        for (unsigned i = 0; i < 32; ++i) {
            const VkDeviceSize size = 4 * (1 + random() % 4);
            copies.push_back({4 * (random() % 120), 4 * (random() % 29), size});
        }
        CheckCopies(copies);
    }
}

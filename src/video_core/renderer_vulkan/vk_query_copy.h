// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <iterator>
#include <map>
#include <span>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Vulkan {

// Pending query writes are ordered. Keep the last writer of each byte, since a single
// vkCmdCopyBuffer must not contain overlapping destination regions.
inline std::vector<VkBufferCopy> NonOverlappingQueryCopies(std::span<const VkBufferCopy> copies) {
    std::map<VkDeviceSize, VkDeviceSize> covered;
    std::vector<VkBufferCopy> result;
    result.reserve(copies.size());
    for (auto copy = copies.rbegin(); copy != copies.rend(); ++copy) {
        const VkDeviceSize end = copy->dstOffset + copy->size;
        VkDeviceSize cursor = copy->dstOffset;
        auto next = covered.lower_bound(cursor);
        if (next != covered.begin() && std::prev(next)->second > cursor) {
            --next;
        }
        const auto append = [&](VkDeviceSize begin, VkDeviceSize finish) {
            if (begin < finish) {
                result.push_back({copy->srcOffset + begin - copy->dstOffset, begin, finish - begin});
            }
        };
        for (; next != covered.end() && next->first < end; ++next) {
            append(cursor, std::min(end, next->first));
            cursor = std::max(cursor, next->second);
        }
        append(cursor, end);

        VkDeviceSize begin = copy->dstOffset;
        VkDeviceSize finish = end;
        next = covered.lower_bound(begin);
        if (next != covered.begin() && std::prev(next)->second >= begin) {
            --next;
        }
        while (next != covered.end() && next->first <= finish) {
            begin = std::min(begin, next->first);
            finish = std::max(finish, next->second);
            next = covered.erase(next);
        }
        covered.emplace_hint(next, begin, finish);
    }
    return result;
}

} // namespace Vulkan

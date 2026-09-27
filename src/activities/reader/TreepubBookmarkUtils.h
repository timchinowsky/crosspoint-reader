#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TreepubBookmarkUtils {

inline void toggleWithCap(std::vector<uint32_t>& bookmarks, const uint32_t nodeId, const size_t maxCount) {
  const auto it = std::find(bookmarks.begin(), bookmarks.end(), nodeId);
  if (it != bookmarks.end()) {
    bookmarks.erase(it);
    return;
  }
  if (maxCount == 0) return;
  if (bookmarks.size() >= maxCount) bookmarks.erase(bookmarks.begin());
  bookmarks.push_back(nodeId);
}

}  // namespace TreepubBookmarkUtils

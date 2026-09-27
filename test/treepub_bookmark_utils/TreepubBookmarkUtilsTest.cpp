#include <gtest/gtest.h>

#include "activities/reader/TreepubBookmarkUtils.h"

namespace {

TEST(TreepubBookmarkUtils, AddsBookmarkWhenAbsent) {
  std::vector<uint32_t> bookmarks = {10, 20};
  TreepubBookmarkUtils::toggleWithCap(bookmarks, 30, 4);
  ASSERT_EQ(bookmarks.size(), 3u);
  EXPECT_EQ(bookmarks[0], 10u);
  EXPECT_EQ(bookmarks[1], 20u);
  EXPECT_EQ(bookmarks[2], 30u);
}

TEST(TreepubBookmarkUtils, RemovesBookmarkWhenPresent) {
  std::vector<uint32_t> bookmarks = {10, 20, 30};
  TreepubBookmarkUtils::toggleWithCap(bookmarks, 20, 4);
  ASSERT_EQ(bookmarks.size(), 2u);
  EXPECT_EQ(bookmarks[0], 10u);
  EXPECT_EQ(bookmarks[1], 30u);
}

TEST(TreepubBookmarkUtils, EvictsOldestAtCapacity) {
  std::vector<uint32_t> bookmarks = {1, 2, 3};
  TreepubBookmarkUtils::toggleWithCap(bookmarks, 4, 3);
  ASSERT_EQ(bookmarks.size(), 3u);
  EXPECT_EQ(bookmarks[0], 2u);
  EXPECT_EQ(bookmarks[1], 3u);
  EXPECT_EQ(bookmarks[2], 4u);
}

TEST(TreepubBookmarkUtils, DoesNotAddWhenCapacityIsZero) {
  std::vector<uint32_t> bookmarks = {1, 2, 3};
  TreepubBookmarkUtils::toggleWithCap(bookmarks, 9, 0);
  ASSERT_EQ(bookmarks.size(), 3u);
  EXPECT_EQ(bookmarks[0], 1u);
  EXPECT_EQ(bookmarks[1], 2u);
  EXPECT_EQ(bookmarks[2], 3u);
}

}  // namespace

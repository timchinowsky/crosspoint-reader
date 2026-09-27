#include <gtest/gtest.h>

#include "TreepubLimits.h"

namespace {

TEST(TreepubLimits, AcceptsNearLimitSourceSize) {
  const size_t nearLimit = TreepubLimits::MAX_SOURCE_BYTES;
  EXPECT_TRUE(TreepubLimits::isValidSourceSize(nearLimit));
  EXPECT_EQ(TreepubLimits::computeDocCapacity(nearLimit), TreepubLimits::MAX_JSON_DOC_BYTES);
}

TEST(TreepubLimits, RejectsOversizedSourceSize) {
  EXPECT_FALSE(TreepubLimits::isValidSourceSize(TreepubLimits::MAX_SOURCE_BYTES + 1));
}

TEST(TreepubLimits, RejectsZeroSizedSource) { EXPECT_FALSE(TreepubLimits::isValidSourceSize(0)); }

}  // namespace

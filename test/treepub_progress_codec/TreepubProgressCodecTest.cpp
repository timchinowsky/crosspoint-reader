#include <gtest/gtest.h>

#include <array>

#include "activities/reader/TreepubProgressCodec.h"

namespace {

TEST(TreepubProgressCodec, RoundTripsNodeAndPage) {
  std::array<uint8_t, TreepubProgressCodec::ENCODED_SIZE> buffer{};
  TreepubProgressCodec::ProgressRecord in{.nodeId = 42, .page = 70000};
  ASSERT_TRUE(TreepubProgressCodec::encode(buffer.data(), buffer.size(), in));

  TreepubProgressCodec::ProgressRecord out;
  ASSERT_TRUE(TreepubProgressCodec::decode(buffer.data(), buffer.size(), &out));
  EXPECT_EQ(out.nodeId, 42u);
  EXPECT_EQ(out.page, 70000u);
}

TEST(TreepubProgressCodec, RejectsWrongVersion) {
  std::array<uint8_t, TreepubProgressCodec::ENCODED_SIZE> buffer{};
  TreepubProgressCodec::ProgressRecord in{.nodeId = 1, .page = 2};
  ASSERT_TRUE(TreepubProgressCodec::encode(buffer.data(), buffer.size(), in));
  buffer[sizeof(uint32_t)] = 1;

  TreepubProgressCodec::ProgressRecord out;
  EXPECT_FALSE(TreepubProgressCodec::decode(buffer.data(), buffer.size(), &out));
}

TEST(TreepubProgressCodec, RejectsShortBuffers) {
  std::array<uint8_t, TreepubProgressCodec::ENCODED_SIZE - 1> shortBuffer{};
  TreepubProgressCodec::ProgressRecord out;
  EXPECT_FALSE(TreepubProgressCodec::decode(shortBuffer.data(), shortBuffer.size(), &out));
}

}  // namespace

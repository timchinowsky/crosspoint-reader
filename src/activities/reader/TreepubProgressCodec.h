#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace TreepubProgressCodec {

struct ProgressRecord {
  uint32_t nodeId = 0;
  uint32_t page = 0;
};

inline constexpr uint32_t MAGIC = 0x50525454;  // "TTRP"
inline constexpr uint8_t VERSION = 3;
inline constexpr size_t ENCODED_SIZE = sizeof(MAGIC) + sizeof(VERSION) + sizeof(uint32_t) + sizeof(uint32_t);

inline bool encode(uint8_t* buffer, const size_t bufferSize, const ProgressRecord& record) {
  if (!buffer || bufferSize < ENCODED_SIZE) return false;
  uint8_t* p = buffer;
  memcpy(p, &MAGIC, sizeof(MAGIC));
  p += sizeof(MAGIC);
  *p++ = VERSION;
  memcpy(p, &record.nodeId, sizeof(record.nodeId));
  p += sizeof(record.nodeId);
  memcpy(p, &record.page, sizeof(record.page));
  return true;
}

inline bool decode(const uint8_t* buffer, const size_t bufferSize, ProgressRecord* out) {
  if (!buffer || !out || bufferSize < ENCODED_SIZE) return false;
  const uint8_t* p = buffer;
  uint32_t magic = 0;
  uint8_t version = 0;
  memcpy(&magic, p, sizeof(magic));
  p += sizeof(magic);
  version = *p++;
  if (magic != MAGIC || version != VERSION) return false;
  memcpy(&out->nodeId, p, sizeof(out->nodeId));
  p += sizeof(out->nodeId);
  memcpy(&out->page, p, sizeof(out->page));
  return true;
}

}  // namespace TreepubProgressCodec

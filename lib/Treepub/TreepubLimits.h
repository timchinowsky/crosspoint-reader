#pragma once

#include <algorithm>
#include <cstddef>

namespace TreepubLimits {

inline constexpr size_t MAX_SOURCE_BYTES = 88 * 1024;  // Keep source-size contract aligned with parser RAM ceiling.
inline constexpr size_t MIN_JSON_DOC_BYTES = 8 * 1024;
inline constexpr size_t MAX_JSON_DOC_BYTES = 48 * 1024;  // ESP32-C3 parser heap guardrail.

inline bool isValidSourceSize(const size_t fileSize) { return fileSize > 0 && fileSize <= MAX_SOURCE_BYTES; }

inline size_t computeDocCapacity(const size_t fileSize) {
  return std::clamp<size_t>((fileSize / 2) + 4096, MIN_JSON_DOC_BYTES, MAX_JSON_DOC_BYTES);
}

}  // namespace TreepubLimits

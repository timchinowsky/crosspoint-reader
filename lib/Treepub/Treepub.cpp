#include "Treepub.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <utility>

namespace {
constexpr uint32_t TREEPUB_CACHE_MAGIC = 0x42505254;  // "TRPB"
constexpr uint8_t TREEPUB_CACHE_VERSION = 1;
constexpr char TREEPUB_CACHE_FILE[] = "/treepub.bin";
constexpr size_t MAX_TREEPUB_FILE_SIZE = 512 * 1024;
}  // namespace

Treepub::Treepub(std::string path, std::string cacheBase)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBase)) {
  cachePath = cacheBasePath + "/treepub_" + std::to_string(std::hash<std::string>{}(filepath));
}

bool Treepub::writeString(HalFile& file, const std::string& value) {
  const uint32_t len = static_cast<uint32_t>(value.size());
  if (file.write(reinterpret_cast<const uint8_t*>(&len), sizeof(len)) != sizeof(len)) return false;
  if (len == 0) return true;
  return file.write(reinterpret_cast<const uint8_t*>(value.data()), len) == len;
}

bool Treepub::readString(HalFile& file, std::string& value) {
  uint32_t len = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len)) return false;
  if (len == 0) {
    value.clear();
    return true;
  }
  auto buffer = makeUniqueNoThrow<char[]>(len + 1);
  if (!buffer) return false;
  if (file.read(reinterpret_cast<uint8_t*>(buffer.get()), len) != static_cast<int>(len)) return false;
  buffer[len] = '\0';
  value.assign(buffer.get(), len);
  return true;
}

void Treepub::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) Storage.mkdir(cacheBasePath.c_str());
  if (!Storage.exists(cachePath.c_str())) Storage.mkdir(cachePath.c_str());
}

bool Treepub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) return true;
  return Storage.removeDir(cachePath.c_str());
}

bool Treepub::buildIndex() {
  idToIndex.clear();
  idToIndex.reserve(nodes.size());
  for (size_t i = 0; i < nodes.size(); i++) {
    const uint32_t id = nodes[i].id;
    if (id == 0) {
      LOG_ERR("TRP", "Node id cannot be 0");
      return false;
    }
    if (idToIndex.find(id) != idToIndex.end()) {
      LOG_ERR("TRP", "Duplicate node id: %u", static_cast<unsigned>(id));
      return false;
    }
    idToIndex[id] = i;
  }
  if (idToIndex.find(rootId) == idToIndex.end()) {
    LOG_ERR("TRP", "Missing root node id: %u", static_cast<unsigned>(rootId));
    return false;
  }
  return true;
}

bool Treepub::loadFromCache() {
  HalFile file;
  if (!Storage.openFileForRead("TRP", cachePath + TREEPUB_CACHE_FILE, file)) return false;

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t nodeCount = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic)) != sizeof(magic)) return false;
  if (file.read(reinterpret_cast<uint8_t*>(&version), sizeof(version)) != sizeof(version)) return false;
  if (magic != TREEPUB_CACHE_MAGIC || version != TREEPUB_CACHE_VERSION) return false;
  if (!readString(file, title) || !readString(file, author)) return false;
  if (file.read(reinterpret_cast<uint8_t*>(&rootId), sizeof(rootId)) != sizeof(rootId)) return false;
  if (file.read(reinterpret_cast<uint8_t*>(&nodeCount), sizeof(nodeCount)) != sizeof(nodeCount)) return false;

  nodes.clear();
  nodes.reserve(nodeCount);
  for (uint32_t i = 0; i < nodeCount; i++) {
    Node node;
    uint32_t childCount = 0;
    if (file.read(reinterpret_cast<uint8_t*>(&node.id), sizeof(node.id)) != sizeof(node.id)) return false;
    if (file.read(reinterpret_cast<uint8_t*>(&node.parentId), sizeof(node.parentId)) != sizeof(node.parentId)) return false;
    if (!readString(file, node.title) || !readString(file, node.text) || !readString(file, node.imagePath)) return false;
    if (file.read(reinterpret_cast<uint8_t*>(&childCount), sizeof(childCount)) != sizeof(childCount)) return false;
    node.children.resize(childCount);
    if (childCount > 0) {
      const size_t bytes = sizeof(uint32_t) * childCount;
      if (file.read(reinterpret_cast<uint8_t*>(node.children.data()), bytes) != static_cast<int>(bytes)) return false;
    }
    nodes.push_back(std::move(node));
  }

  return buildIndex();
}

bool Treepub::saveToCache() const {
  HalFile file;
  if (!Storage.openFileForWrite("TRP", cachePath + TREEPUB_CACHE_FILE, file)) return false;

  if (file.write(reinterpret_cast<const uint8_t*>(&TREEPUB_CACHE_MAGIC), sizeof(TREEPUB_CACHE_MAGIC)) !=
      sizeof(TREEPUB_CACHE_MAGIC))
    return false;
  if (file.write(&TREEPUB_CACHE_VERSION, sizeof(TREEPUB_CACHE_VERSION)) != sizeof(TREEPUB_CACHE_VERSION)) return false;
  if (!writeString(file, title) || !writeString(file, author)) return false;
  if (file.write(reinterpret_cast<const uint8_t*>(&rootId), sizeof(rootId)) != sizeof(rootId)) return false;
  const uint32_t nodeCount = static_cast<uint32_t>(nodes.size());
  if (file.write(reinterpret_cast<const uint8_t*>(&nodeCount), sizeof(nodeCount)) != sizeof(nodeCount)) return false;

  for (const auto& node : nodes) {
    if (file.write(reinterpret_cast<const uint8_t*>(&node.id), sizeof(node.id)) != sizeof(node.id)) return false;
    if (file.write(reinterpret_cast<const uint8_t*>(&node.parentId), sizeof(node.parentId)) != sizeof(node.parentId))
      return false;
    if (!writeString(file, node.title) || !writeString(file, node.text) || !writeString(file, node.imagePath))
      return false;
    const uint32_t childCount = static_cast<uint32_t>(node.children.size());
    if (file.write(reinterpret_cast<const uint8_t*>(&childCount), sizeof(childCount)) != sizeof(childCount)) return false;
    if (childCount > 0) {
      const size_t bytes = sizeof(uint32_t) * childCount;
      if (file.write(reinterpret_cast<const uint8_t*>(node.children.data()), bytes) != bytes) return false;
    }
  }

  return true;
}

bool Treepub::parseSourceFile() {
  HalFile file;
  if (!Storage.openFileForRead("TRP", filepath, file)) {
    LOG_ERR("TRP", "Failed to open treepub: %s", filepath.c_str());
    return false;
  }

  const size_t fileSize = file.size();
  if (fileSize == 0 || fileSize > MAX_TREEPUB_FILE_SIZE) {
    LOG_ERR("TRP", "Invalid treepub size: %u", static_cast<unsigned>(fileSize));
    return false;
  }

  auto buffer = makeUniqueNoThrow<char[]>(fileSize + 1);
  if (!buffer) {
    LOG_ERR("TRP", "OOM parsing treepub (%u bytes)", static_cast<unsigned>(fileSize));
    return false;
  }
  if (file.read(reinterpret_cast<uint8_t*>(buffer.get()), fileSize) != static_cast<int>(fileSize)) {
    LOG_ERR("TRP", "Failed to read treepub file");
    return false;
  }
  buffer[fileSize] = '\0';

  const size_t docCapacity = fileSize * 2 + 4096;
  DynamicJsonDocument doc(docCapacity);
  const auto err = deserializeJson(doc, buffer.get());
  if (err) {
    LOG_ERR("TRP", "Treepub JSON parse error: %s", err.c_str());
    return false;
  }

  title = doc["title"] | "";
  author = doc["author"] | "";
  rootId = doc["root"] | 0;
  if (rootId == 0) {
    LOG_ERR("TRP", "Treepub root id missing");
    return false;
  }

  JsonArray nodeArray = doc["nodes"].as<JsonArray>();
  if (nodeArray.isNull() || nodeArray.size() == 0) {
    LOG_ERR("TRP", "Treepub has no nodes");
    return false;
  }

  nodes.clear();
  nodes.reserve(nodeArray.size());
  for (JsonVariant item : nodeArray) {
    JsonObject obj = item.as<JsonObject>();
    if (obj.isNull()) continue;

    Node node;
    node.id = obj["id"] | 0;
    node.parentId = obj["parent"] | 0;
    node.title = obj["title"] | "";
    node.text = obj["text"] | "";
    node.imagePath = obj["image"] | "";
    if (node.title.empty()) node.title = std::to_string(node.id);

    JsonArray children = obj["children"].as<JsonArray>();
    if (!children.isNull()) {
      node.children.reserve(children.size());
      for (JsonVariant child : children) {
        const uint32_t childId = child.as<uint32_t>();
        if (childId != 0) node.children.push_back(childId);
      }
    }
    nodes.push_back(std::move(node));
  }

  if (!buildIndex()) return false;

  // Fill missing parent links from child edges.
  for (const auto& node : nodes) {
    for (const uint32_t childId : node.children) {
      auto it = idToIndex.find(childId);
      if (it == idToIndex.end()) continue;
      Node& child = nodes[it->second];
      if (child.parentId == 0) child.parentId = node.id;
    }
  }

  return true;
}

bool Treepub::load() {
  if (loaded) return true;
  setupCacheDir();
  if (!loadFromCache() && !parseSourceFile()) return false;
  saveToCache();
  loaded = true;
  return true;
}

const Treepub::Node* Treepub::getNode(const uint32_t nodeId) const {
  const auto it = idToIndex.find(nodeId);
  if (it == idToIndex.end()) return nullptr;
  return &nodes[it->second];
}

std::optional<uint32_t> Treepub::getParent(const uint32_t nodeId) const {
  const Node* node = getNode(nodeId);
  if (!node || node->parentId == 0) return std::nullopt;
  if (!getNode(node->parentId)) return std::nullopt;
  return node->parentId;
}

std::optional<uint32_t> Treepub::getNextSibling(const uint32_t nodeId) const {
  const auto parentId = getParent(nodeId);
  if (!parentId.has_value()) return std::nullopt;
  const Node* parent = getNode(parentId.value());
  if (!parent) return std::nullopt;
  for (size_t i = 0; i < parent->children.size(); i++) {
    if (parent->children[i] != nodeId) continue;
    if (i + 1 < parent->children.size()) return parent->children[i + 1];
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<uint32_t> Treepub::getPrevSibling(const uint32_t nodeId) const {
  const auto parentId = getParent(nodeId);
  if (!parentId.has_value()) return std::nullopt;
  const Node* parent = getNode(parentId.value());
  if (!parent) return std::nullopt;
  for (size_t i = 0; i < parent->children.size(); i++) {
    if (parent->children[i] != nodeId) continue;
    if (i > 0) return parent->children[i - 1];
    return std::nullopt;
  }
  return std::nullopt;
}

std::vector<uint32_t> Treepub::getPathToRoot(uint32_t nodeId) const {
  std::vector<uint32_t> out;
  const Node* node = getNode(nodeId);
  while (node) {
    out.push_back(node->id);
    if (node->parentId == 0) break;
    node = getNode(node->parentId);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::optional<uint32_t> Treepub::findNextBranchNode(uint32_t nodeId) const {
  uint32_t cursor = nodeId;
  while (true) {
    const auto nextSibling = getNextSibling(cursor);
    if (nextSibling.has_value()) return nextSibling;
    const auto parent = getParent(cursor);
    if (!parent.has_value()) return std::nullopt;
    cursor = parent.value();
  }
}

#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Treepub {
 public:
  struct Node {
    uint32_t id = 0;
    uint32_t parentId = 0;
    std::string title;
    std::string text;
    std::string imagePath;
    std::vector<uint32_t> children;
  };

 private:
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  std::string title;
  std::string author;
  uint32_t rootId = 0;
  bool loaded = false;

  std::vector<Node> nodes;
  std::unordered_map<uint32_t, size_t> idToIndex;

  bool loadFromCache();
  bool saveToCache() const;
  bool parseSourceFile();
  bool buildIndex();

  static bool writeString(HalFile& file, const std::string& value);
  static bool readString(HalFile& file, std::string& value);

 public:
  explicit Treepub(std::string path, std::string cacheBasePath);

  bool load();
  void setupCacheDir() const;
  bool clearCache() const;

  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] const std::string& getTitle() const { return title; }
  [[nodiscard]] const std::string& getAuthor() const { return author; }
  [[nodiscard]] uint32_t getRootId() const { return rootId; }
  [[nodiscard]] bool isLoaded() const { return loaded; }

  [[nodiscard]] const Node* getNode(uint32_t nodeId) const;
  [[nodiscard]] std::optional<uint32_t> getNextSibling(uint32_t nodeId) const;
  [[nodiscard]] std::optional<uint32_t> getPrevSibling(uint32_t nodeId) const;
  [[nodiscard]] std::optional<uint32_t> getParent(uint32_t nodeId) const;
  [[nodiscard]] std::vector<uint32_t> getPathToRoot(uint32_t nodeId) const;
  [[nodiscard]] std::optional<uint32_t> findNextBranchNode(uint32_t nodeId) const;
  [[nodiscard]] size_t getNodeCount() const { return nodes.size(); }
  [[nodiscard]] const std::vector<Node>& getNodes() const { return nodes; }
};

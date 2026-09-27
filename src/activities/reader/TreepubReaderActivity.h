#pragma once

#include <Treepub.h>

#include <memory>
#include <string>
#include <vector>

#include "ReaderActivity.h"

class TreepubReaderActivity final : public ReaderActivity {
  std::unique_ptr<Treepub> treepub;
  uint32_t currentNodeId = 0;
  int currentPage = 0;
  int totalPages = 1;
  std::vector<uint32_t> history;
  std::vector<uint32_t> bookmarks;

  std::vector<std::string> wrappedLines;
  int linesPerPage = 1;
  int orientedMarginTop = 0;
  int orientedMarginRight = 0;
  int orientedMarginBottom = 0;
  int orientedMarginLeft = 0;
  int viewportWidth = 0;
  bool nodeIsImage = false;
  std::string cachedBreadcrumb;
  int cachedDepth = 1;

  std::string lastHint;
  unsigned long lastHintAt = 0;
  static constexpr unsigned long HINT_DURATION_MS = 1500;
  static constexpr size_t HISTORY_MAX = 64;
  static constexpr size_t BOOKMARK_MAX = 128;

  void updateLayoutMetrics();
  void buildNodeLayout();
  void renderNode();
  void renderStatusBar() const;
  void renderHint() const;
  bool navigateToNode(uint32_t nodeId, bool pushHistory, const char* hintText = nullptr);
  void openNavigator();
  void saveProgress() const;
  void loadProgress();
  void loadBookmarks();
  void saveBookmarks() const;
  void toggleBookmark();
  int indexInSiblings(const Treepub::Node& node, int* totalSiblings) const;
  void refreshPositionCache();
  std::string breadcrumb() const;

  bool loadBook() override;
  std::string getBookTitle() const override { return treepub ? treepub->getTitle() : ""; }
  std::string getBookAuthor() const override { return treepub ? treepub->getAuthor() : ""; }
  bool handleFormatInput() override;
  void renderBook() override;

 public:
  explicit TreepubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                                 bool allowFastInitialRefresh)
      : ReaderActivity("TreepubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  ScreenshotInfo getScreenshotInfo() const override;
};

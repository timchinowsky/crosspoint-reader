#include "TreepubReaderActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "TreepubBookmarkUtils.h"
#include "TreepubNavigatorActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t TREEPUB_PROGRESS_MAGIC = 0x50525454;  // "TTRP"
constexpr uint8_t TREEPUB_PROGRESS_VERSION = 2;
constexpr uint32_t TREEPUB_BOOKMARK_MAGIC = 0x4B525454;  // "TTRK"
constexpr uint8_t TREEPUB_BOOKMARK_VERSION = 1;
}  // namespace

bool TreepubReaderActivity::loadBook() {
  auto loadedTreepub = makeUniqueNoThrow<Treepub>(bookPath, "/.crosspoint");
  if (!loadedTreepub) {
    LOG_ERR("TRR", "Failed to allocate Treepub object");
    return false;
  }
  if (!loadedTreepub->load()) {
    LOG_ERR("TRR", "Failed to load treepub");
    return false;
  }
  treepub = std::shared_ptr<Treepub>(std::move(loadedTreepub));
  treepub->setupCacheDir();
  history.reserve(HISTORY_MAX);
  bookmarks.reserve(BOOKMARK_MAX);
  wrappedLines.reserve(64);
  currentNodeId = treepub->getRootId();
  loadProgress();
  loadBookmarks();
  refreshPositionCache();
  return treepub->getNode(currentNodeId) != nullptr;
}

void TreepubReaderActivity::updateLayoutMetrics() {
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom +=
      std::max(SETTINGS.screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));
  viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  linesPerPage = std::max(1, viewportHeight / renderer.getLineHeight(SETTINGS.getReaderFontId()));
}

void TreepubReaderActivity::buildNodeLayout() {
  wrappedLines.clear();
  totalPages = 1;
  currentPage = std::max(0, currentPage);
  updateLayoutMetrics();
  const Treepub::Node* node = treepub ? treepub->getNode(currentNodeId) : nullptr;
  if (!node) return;
  nodeIsImage = !node->imagePath.empty() && FsHelpers::hasBmpExtension(node->imagePath);
  if (nodeIsImage) {
    totalPages = 1;
    return;
  }

  const std::string& contentRef = node->text.empty() ? node->title : node->text;
  const int fontId = SETTINGS.getReaderFontId();
  const size_t expectedLines = std::max<size_t>(16, contentRef.size() / 48);
  wrappedLines.reserve(std::min<size_t>(expectedLines, 1024));
  auto lines = renderer.wrappedText(fontId, contentRef.c_str(), viewportWidth, 4096, EpdFontFamily::REGULAR);
  wrappedLines.insert(wrappedLines.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
  if (wrappedLines.empty()) wrappedLines.push_back("");
  totalPages = std::max(1, static_cast<int>((wrappedLines.size() + linesPerPage - 1) / linesPerPage));
  if (currentPage >= totalPages) currentPage = totalPages - 1;
}

void TreepubReaderActivity::refreshPositionCache() {
  if (!treepub) {
    cachedDepth = 1;
    cachedBreadcrumb.clear();
    return;
  }
  std::vector<uint32_t> ids = treepub->getPathToRoot(currentNodeId);
  cachedDepth = std::max(1, static_cast<int>(ids.size()));
  cachedBreadcrumb.clear();
  for (size_t i = 0; i < ids.size(); i++) {
    const Treepub::Node* node = treepub->getNode(ids[i]);
    if (!node) continue;
    if (!cachedBreadcrumb.empty()) cachedBreadcrumb += " > ";
    cachedBreadcrumb += node->title;
  }
  constexpr size_t MAX_BREADCRUMB = 52;
  if (cachedBreadcrumb.size() > MAX_BREADCRUMB) {
    cachedBreadcrumb = "..." + cachedBreadcrumb.substr(cachedBreadcrumb.size() - (MAX_BREADCRUMB - 3));
  }
}

std::string TreepubReaderActivity::breadcrumb() const { return cachedBreadcrumb; }

int TreepubReaderActivity::indexInSiblings(const Treepub::Node& node, int* totalSiblings) const {
  if (!treepub) {
    *totalSiblings = 1;
    return 1;
  }
  const auto parentId = treepub->getParent(node.id);
  if (!parentId.has_value()) {
    *totalSiblings = 1;
    return 1;
  }
  const Treepub::Node* parent = treepub->getNode(parentId.value());
  if (!parent || parent->children.empty()) {
    *totalSiblings = 1;
    return 1;
  }
  *totalSiblings = static_cast<int>(parent->children.size());
  for (size_t i = 0; i < parent->children.size(); i++) {
    if (parent->children[i] == node.id) return static_cast<int>(i + 1);
  }
  return 1;
}

void TreepubReaderActivity::renderStatusBar() const {
  if (!treepub) return;
  const Treepub::Node* node = treepub->getNode(currentNodeId);
  if (!node) return;

  int siblingCount = 1;
  const int siblingIndex = indexInSiblings(*node, &siblingCount);
  char suffix[64];
  snprintf(suffix, sizeof(suffix), " d%d %d/%d p%d/%d", cachedDepth, siblingIndex, siblingCount, currentPage + 1,
           totalPages);
  std::string titleText = breadcrumb();
  titleText += suffix;

  GUI.drawStatusBar(renderer, 0.0f, currentPage + 1, totalPages, titleText);
}

void TreepubReaderActivity::renderHint() const {
  if (lastHint.empty()) return;
  if (millis() - lastHintAt > HINT_DURATION_MS) return;
  renderer.drawCenteredText(UI_10_FONT_ID,
                            renderer.getScreenHeight() - UITheme::getInstance().getStatusBarHeight() - 10,
                            lastHint.c_str(), true, EpdFontFamily::BOLD);
}

void TreepubReaderActivity::renderNode() {
  const Treepub::Node* node = treepub ? treepub->getNode(currentNodeId) : nullptr;
  if (!node) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR), true,
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  if (nodeIsImage) {
    HalFile file;
    if (Storage.openFileForRead("TRR", node->imagePath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const int pageWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
        const int pageHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
        const int x = std::max(orientedMarginLeft, (renderer.getScreenWidth() - bitmap.getWidth()) / 2);
        const int y = std::max(orientedMarginTop, (renderer.getScreenHeight() - bitmap.getHeight()) / 2);
        renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
      } else {
        renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_INVALID_BMP_FILE), true,
                                  EpdFontFamily::BOLD);
      }
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_FILE_OPEN_FAILED), true,
                                EpdFontFamily::BOLD);
    }
  } else {
    const int fontId = SETTINGS.getReaderFontId();
    const int lineHeight = renderer.getLineHeight(fontId);
    const int startLine = currentPage * linesPerPage;
    const int endLine = std::min(static_cast<int>(wrappedLines.size()), startLine + linesPerPage);
    int y = orientedMarginTop;
    for (int i = startLine; i < endLine; i++) {
      renderer.drawText(fontId, orientedMarginLeft, y, wrappedLines[i].c_str(), true, EpdFontFamily::REGULAR);
      y += lineHeight;
    }
  }

  renderStatusBar();
  renderHint();
  renderer.displayBuffer();
}

bool TreepubReaderActivity::navigateToNode(const uint32_t nodeId, const bool pushHistory, const char* hintText) {
  if (!treepub || !treepub->getNode(nodeId)) return false;
  if (nodeId == currentNodeId) return true;
  if (pushHistory) {
    if (history.size() >= HISTORY_MAX) history.erase(history.begin());
    history.push_back(currentNodeId);
  }
  currentNodeId = nodeId;
  currentPage = 0;
  refreshPositionCache();
  buildNodeLayout();
  if (hintText) {
    lastHint = hintText;
    lastHintAt = millis();
  }
  saveProgress();
  return true;
}

void TreepubReaderActivity::openNavigator() {
  startActivityForResult(
      std::make_unique<TreepubNavigatorActivity>(renderer, mappedInput, treepub, currentNodeId, history),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto data = std::get<TreepubNavigatorActivity::Result>(result.data);
        navigateToNode(data.nodeId, true, tr(STR_TREEPUB_MOVED));
        requestUpdate();
      });
}

bool TreepubReaderActivity::handleFormatInput() {
  if (!treepub) return false;
  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const Treepub::Node* node = treepub->getNode(currentNodeId);
    if (!node) return false;
    if (!node->children.empty()) {
      openNavigator();
    } else {
      toggleBookmark();
      lastHint = tr(STR_BOOKMARK_OPTION);
      lastHintAt = millis();
      requestUpdate();
    }
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!history.empty()) {
      const uint32_t prev = history.back();
      history.pop_back();
      navigateToNode(prev, false, tr(STR_TREEPUB_RETURNED));
      requestUpdate();
      return true;
    }
    if (const auto parentId = treepub->getParent(currentNodeId); parentId.has_value()) {
      navigateToNode(parentId.value(), true, tr(STR_TREEPUB_RETURNED));
      requestUpdate();
      return true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (const auto prevSibling = treepub->getPrevSibling(currentNodeId); prevSibling.has_value()) {
      navigateToNode(prevSibling.value(), true, tr(STR_TREEPUB_MOVED));
      requestUpdate();
      return true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (const auto nextSibling = treepub->getNextSibling(currentNodeId); nextSibling.has_value()) {
      navigateToNode(nextSibling.value(), true, tr(STR_TREEPUB_MOVED));
      requestUpdate();
      return true;
    }
  }

  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 1000)) {
    toggleBookmark();
    lastHint = tr(STR_BOOKMARK_OPTION);
    lastHintAt = millis();
    requestUpdate();
    return true;
  }
  return false;
}

void TreepubReaderActivity::saveProgress() const {
  if (!treepub) return;
  const size_t payloadBytes =
      sizeof(TREEPUB_PROGRESS_MAGIC) + sizeof(TREEPUB_PROGRESS_VERSION) + sizeof(currentNodeId) + sizeof(uint16_t);
  auto data = makeUniqueNoThrow<uint8_t[]>(payloadBytes);
  if (!data) {
    LOG_ERR("TRR", "OOM writing treepub progress");
    return;
  }
  uint8_t* p = data.get();
  memcpy(p, &TREEPUB_PROGRESS_MAGIC, sizeof(TREEPUB_PROGRESS_MAGIC));
  p += sizeof(TREEPUB_PROGRESS_MAGIC);
  *p++ = TREEPUB_PROGRESS_VERSION;
  memcpy(p, &currentNodeId, sizeof(currentNodeId));
  p += sizeof(currentNodeId);
  const uint16_t page = static_cast<uint16_t>(std::max(0, currentPage));
  memcpy(p, &page, sizeof(page));
  ProgressFile::writeAtomic(treepub->getCachePath(), data.get(), payloadBytes);
}

void TreepubReaderActivity::loadProgress() {
  if (!treepub) return;
  HalFile file;
  if (!Storage.openFileForRead("TRR", treepub->getCachePath() + "/progress.bin", file)) return;
  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t nodeId = 0;
  uint16_t page = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic)) != sizeof(magic)) return;
  if (file.read(&version, sizeof(version)) != sizeof(version)) return;
  if (magic != TREEPUB_PROGRESS_MAGIC || version != TREEPUB_PROGRESS_VERSION) return;
  if (file.read(reinterpret_cast<uint8_t*>(&nodeId), sizeof(nodeId)) != sizeof(nodeId)) return;
  if (file.read(reinterpret_cast<uint8_t*>(&page), sizeof(page)) != sizeof(page)) return;
  if (treepub->getNode(nodeId)) {
    currentNodeId = nodeId;
    currentPage = page;
  }
  history.clear();
  refreshPositionCache();
  buildNodeLayout();
}

void TreepubReaderActivity::saveBookmarks() const {
  if (!treepub) return;
  const uint16_t count = static_cast<uint16_t>(bookmarks.size());
  const size_t payloadBytes =
      sizeof(TREEPUB_BOOKMARK_MAGIC) + sizeof(TREEPUB_BOOKMARK_VERSION) + sizeof(count) + sizeof(uint32_t) * count;
  auto data = makeUniqueNoThrow<uint8_t[]>(payloadBytes);
  if (!data) {
    LOG_ERR("TRR", "OOM writing treepub bookmarks");
    return;
  }
  uint8_t* p = data.get();
  memcpy(p, &TREEPUB_BOOKMARK_MAGIC, sizeof(TREEPUB_BOOKMARK_MAGIC));
  p += sizeof(TREEPUB_BOOKMARK_MAGIC);
  *p++ = TREEPUB_BOOKMARK_VERSION;
  memcpy(p, &count, sizeof(count));
  p += sizeof(count);
  if (count > 0) memcpy(p, bookmarks.data(), sizeof(uint32_t) * count);

  const std::string finalPath = treepub->getCachePath() + "/bookmarks.bin";
  const std::string tmpPath = treepub->getCachePath() + "/bookmarks.bin.tmp";
  {
    HalFile file;
    if (!Storage.openFileForWrite("TRR", tmpPath, file)) return;
    if (file.write(data.get(), payloadBytes) != payloadBytes) return;
    file.flush();
  }
  Storage.remove(finalPath.c_str());
  Storage.rename(tmpPath.c_str(), finalPath.c_str());
}

void TreepubReaderActivity::loadBookmarks() {
  bookmarks.clear();
  if (!treepub) return;
  const std::string path = treepub->getCachePath() + "/bookmarks.bin";
  HalFile file;
  if (!Storage.openFileForRead("TRR", path, file)) return;
  uint32_t magic = 0;
  uint8_t version = 0;
  uint16_t count = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&magic), sizeof(magic)) != sizeof(magic)) return;
  if (file.read(&version, sizeof(version)) != sizeof(version)) return;
  if (magic != TREEPUB_BOOKMARK_MAGIC || version != TREEPUB_BOOKMARK_VERSION) return;
  if (file.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) return;
  count = std::min<uint16_t>(count, static_cast<uint16_t>(BOOKMARK_MAX));
  bookmarks.resize(count);
  if (count > 0) {
    const size_t bytes = sizeof(uint32_t) * count;
    if (file.read(reinterpret_cast<uint8_t*>(bookmarks.data()), bytes) != static_cast<int>(bytes)) bookmarks.clear();
  }
}

void TreepubReaderActivity::toggleBookmark() {
  TreepubBookmarkUtils::toggleWithCap(bookmarks, currentNodeId, BOOKMARK_MAX);
  saveBookmarks();
}

void TreepubReaderActivity::renderBook() {
  if (!treepub) return;
  buildNodeLayout();
  renderNode();
  saveProgress();
}

bool TreepubReaderActivity::pageTurn(const bool isForward) {
  buildNodeLayout();
  if (!treepub) return false;
  if (!nodeIsImage) {
    if (isForward && currentPage + 1 < totalPages) {
      currentPage++;
      return true;
    }
    if (!isForward && currentPage > 0) {
      currentPage--;
      return true;
    }
  }

  if (isForward) {
    if (const auto nextSibling = treepub->getNextSibling(currentNodeId); nextSibling.has_value()) {
      return navigateToNode(nextSibling.value(), true, tr(STR_TREEPUB_MOVED));
    }
    if (const auto nextBranch = treepub->findNextBranchNode(currentNodeId); nextBranch.has_value()) {
      return navigateToNode(nextBranch.value(), true, tr(STR_TREEPUB_MOVED));
    }
  } else {
    if (const auto prevSibling = treepub->getPrevSibling(currentNodeId); prevSibling.has_value()) {
      bool moved = navigateToNode(prevSibling.value(), true, tr(STR_TREEPUB_MOVED));
      if (moved) {
        buildNodeLayout();
        currentPage = totalPages - 1;
      }
      return moved;
    }
    if (const auto parent = treepub->getParent(currentNodeId); parent.has_value()) {
      bool moved = navigateToNode(parent.value(), true, tr(STR_TREEPUB_RETURNED));
      if (moved) {
        buildNodeLayout();
        currentPage = totalPages - 1;
      }
      return moved;
    }
  }
  return false;
}

bool TreepubReaderActivity::skipPages(const int amount) {
  if (amount == 0) return false;
  return pageTurn(amount > 0);
}

bool TreepubReaderActivity::isAtEndOfBook() const {
  if (!treepub) return true;
  const Treepub::Node* node = treepub->getNode(currentNodeId);
  if (!node) return true;
  if (!nodeIsImage && currentPage + 1 < totalPages) return false;
  return !treepub->findNextBranchNode(currentNodeId).has_value() && !treepub->getNextSibling(currentNodeId).has_value();
}

void TreepubReaderActivity::onReturnFromEndOfBook() {
  if (const auto parent = treepub->getParent(currentNodeId); parent.has_value()) {
    navigateToNode(parent.value(), true, tr(STR_TREEPUB_RETURNED));
  } else {
    navigateToNode(treepub->getRootId(), false, tr(STR_RESUME));
  }
}

ScreenshotInfo TreepubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.showStatusBar = true;
  info.title = treepub ? treepub->getTitle() : "";
  return info;
}

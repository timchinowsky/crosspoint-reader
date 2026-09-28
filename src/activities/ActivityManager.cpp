#include "ActivityManager.h"

#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Memory.h>
#include <VectorFontSupport.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/HeaderBackTapTarget.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "library/LibraryListActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/UsbDriveActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/BmpViewerActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

namespace {
constexpr size_t FILE_NAME_BUFFER_SIZE = 500;
constexpr size_t MAX_TREE_TRAVERSAL_DEPTH = 32;

bool isReadableFileInTree(std::string_view name) {
  return FsHelpers::hasEpubExtension(name) || FsHelpers::hasXtcExtension(name) || FsHelpers::hasTxtExtension(name) ||
         FsHelpers::hasMarkdownExtension(name) || FsHelpers::hasBmpExtension(name) || FsHelpers::hasPngExtension(name);
}

std::string joinPath(const std::string& base, const std::string& entryName) {
  return base == "/" ? "/" + entryName : base + "/" + entryName;
}

bool isPathInsideBase(const std::string& basePath, const std::string& path) {
  if (basePath == "/") return !path.empty() && path[0] == '/';
  if (path.size() <= basePath.size() || path.compare(0, basePath.size(), basePath) != 0) return false;
  return path[basePath.size()] == '/';
}

bool isPathInsideBase(const char* basePath, const char* path) {
  return isPathInsideBase(std::string(basePath), std::string(path));
}

bool loadTreeEntries(const std::string& path, std::vector<std::string>& entries) {
  entries.clear();
  auto dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("ACT", "Cannot open tree folder: %s", path.c_str());
    return false;
  }

  auto nameBuffer = makeUniqueNoThrow<char[]>(FILE_NAME_BUFFER_SIZE);
  if (!nameBuffer) {
    LOG_ERR("ACT", "OOM: %d bytes", static_cast<int>(FILE_NAME_BUFFER_SIZE));
    return false;
  }

  auto includeEntry = [&](const char* name, bool isDirectory) {
    if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
      return false;
    }
    return isDirectory || isReadableFileInTree(name);
  };

  dir.rewindDirectory();
  size_t entryCount = 0;
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(nameBuffer.get(), FILE_NAME_BUFFER_SIZE);
    if (includeEntry(nameBuffer.get(), file.isDirectory())) ++entryCount;
  }

  entries.reserve(entryCount);
  dir.rewindDirectory();
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(nameBuffer.get(), FILE_NAME_BUFFER_SIZE);
    const bool isDirectory = file.isDirectory();
    if (!includeEntry(nameBuffer.get(), isDirectory)) continue;
    if (isDirectory) {
      entries.emplace_back(std::string(nameBuffer.get()) + "/");
      continue;
    }
    entries.emplace_back(nameBuffer.get());
  }
  FsHelpers::sortFileList(entries);
  return true;
}
}  // namespace

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
#if CROSSPOINT_VECTOR_FONTS
  // FreeType rasterization runs on this task, and the deepest observed chain
  // is a glyph fault DURING LAYOUT: expat + parser + line-layout frames
  // (~3.5KB on Xtensa) with the scan converter's FT_RENDER_POOL_SIZE (4KB)
  // stack-resident band pool on top — a measured ~8KB peak that trips the
  // canary on an 8KB stack. Vector-font boards all have PSRAM-class RAM.
  constexpr uint32_t renderTaskStackBytes = 16384;
#else
  constexpr uint32_t renderTaskStackBytes = 8192;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          renderTaskStackBytes,  // Stack size
                          this,                  // Parameters
                          1,                     // Priority
                          &renderTaskHandle,     // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Night mode is a global output polarity applied to every activity.
      // The sleep screen forces normal polarity itself (SleepActivity).
      display.setInverted(SETTINGS.screenInverted != 0);
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (mappedInput.consumeSuppressedRelease()) return;

  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // An exclusive-storage activity must restart rather than navigate away:
    // processing a pending action here could re-enable filesystem users while
    // the USB host still owns the raw SD card.
    if (requestedUpdate.exchange(false) && renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
    return;
  }

  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Tap-first control-center entry: a tap on the status-bar band of the
    // top-level tab screens opens it, mirroring the top-edge swipe (which some
    // panels' etched glass makes unreliable). The reader keeps its clean page
    // (no status bar there to tap). Touch boards only, like the swipe itself.
    bool statusBarTap = false;
    if (mappedInput.hasTouch() &&
        (currentActivity->name == "Home" || currentActivity->name == "FileBrowser" ||
         currentActivity->name == "Settings" || currentActivity->name == "NetworkModeSelection")) {
      int tx = 0;
      int ty = 0;
      // The header back button shares this band; its taps stay Back.
      statusBarTap = mappedInput.wasScreenTapped(tx, ty) && ty < 44 && !HeaderBackTapTarget::contains(tx, ty);
    }
    if (currentActivity->name != "FrontlightPanel" && (statusBarTap || mappedInput.wasLightPanelGesture())) {
      pushActivity(std::make_unique<FrontlightPanelActivity>(renderer, mappedInput));
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        // The parent's header back rect must not route taps on the pushed
        // screen (which may draw no header of its own).
        HeaderBackTapTarget::clear();
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
  // The outgoing screen's header back button must not eat taps on the next
  // screen; the next header draw re-records it.
  HeaderBackTapTarget::clear();
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  mappedInput.resetHomeButtonInput();
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToUsbDrive() {
#if FREEINK_CAP_USB_MSC
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

void ActivityManager::goToSettings() { replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput)); }

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToLibrary() {
  auto activity = makeUniqueNoThrow<LibraryListActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: library activity");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh, const bool fromTreeSession) {
  if (path.empty()) {
    clearTreeReadingSession();
    goToFileBrowser("/");
    return;
  }
  if (!fromTreeSession) {
    clearTreeReadingSession();
  } else {
    const std::string normalizedPath = FsHelpers::normalisePath(path);
    taskENTER_CRITICAL(&activityManagerSpinlock);
    if (treeReadingSession.active) {
      snprintf(treeReadingSession.currentPath, sizeof(treeReadingSession.currentPath), "%s", normalizedPath.c_str());
    }
    taskEXIT_CRITICAL(&activityManagerSpinlock);
  }

  if (FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path)) {
    auto activity = makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, std::move(path));
    if (!activity) {
      LOG_ERR("ACT", "OOM: bitmap viewer activity");
      return;
    }
    replaceActivity(std::move(activity));
    return;
  }

  auto activity = ReaderActivity::create(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  if (activity) {
    replaceActivity(std::move(activity));
  }
}

void ActivityManager::setTreeReadingSession(std::string basePath, std::string currentPath) {
  const std::string safeBase = FsHelpers::normalisePath(basePath.empty() ? "/" : basePath);
  const std::string safeCurrent = FsHelpers::normalisePath(currentPath);
  taskENTER_CRITICAL(&activityManagerSpinlock);
  const int baseLen =
      snprintf(treeReadingSession.basePath, sizeof(treeReadingSession.basePath), "%s", safeBase.c_str());
  const int currentLen =
      snprintf(treeReadingSession.currentPath, sizeof(treeReadingSession.currentPath), "%s", safeCurrent.c_str());
  treeReadingSession.active = baseLen > 0 && currentLen > 0 &&
                              baseLen < static_cast<int>(sizeof(treeReadingSession.basePath)) &&
                              currentLen < static_cast<int>(sizeof(treeReadingSession.currentPath));
  taskEXIT_CRITICAL(&activityManagerSpinlock);
}

void ActivityManager::clearTreeReadingSession() {
  taskENTER_CRITICAL(&activityManagerSpinlock);
  treeReadingSession.active = false;
  treeReadingSession.basePath[0] = '\0';
  treeReadingSession.currentPath[0] = '\0';
  taskEXIT_CRITICAL(&activityManagerSpinlock);
}

bool ActivityManager::openNextTreeDocument(const std::string& currentPath) {
  const std::string normalizedCurrentPath = FsHelpers::normalisePath(currentPath);
  char basePath[TreeReadingSession::PATH_BUFFER_SIZE]{};
  char sessionCurrentPath[TreeReadingSession::PATH_BUFFER_SIZE]{};
  taskENTER_CRITICAL(&activityManagerSpinlock);
  const bool active = treeReadingSession.active;
  if (active) {
    snprintf(basePath, sizeof(basePath), "%s", treeReadingSession.basePath);
    snprintf(sessionCurrentPath, sizeof(sessionCurrentPath), "%s", treeReadingSession.currentPath);
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  if (!active || basePath[0] == '\0') return false;
  if (normalizedCurrentPath != sessionCurrentPath) {
    clearTreeReadingSession();
    return false;
  }
  if (!isPathInsideBase(basePath, normalizedCurrentPath.c_str()) &&
      !(strcmp(basePath, "/") == 0 && !normalizedCurrentPath.empty() && normalizedCurrentPath[0] == '/')) {
    clearTreeReadingSession();
    return false;
  }

  struct DirState {
    std::string path;
    std::vector<std::string> entries;
    size_t nextEntry = 0;
  };

  std::vector<DirState> stack;
  stack.reserve(MAX_TREE_TRAVERSAL_DEPTH);
  DirState root{basePath, {}, 0};
  if (!loadTreeEntries(root.path, root.entries)) {
    return false;
  }
  stack.push_back(std::move(root));

  const std::string basePathStr{basePath};
  const std::string relative =
      basePathStr == "/" ? normalizedCurrentPath.substr(1) : normalizedCurrentPath.substr(basePathStr.size() + 1);
  if (relative.empty()) return false;
  const size_t slash = relative.find_last_of('/');
  const std::string targetFile = slash == std::string::npos ? relative : relative.substr(slash + 1);
  const std::string parentRelative = slash == std::string::npos ? "" : relative.substr(0, slash);

  size_t start = 0;
  while (start < parentRelative.size()) {
    if (stack.size() >= MAX_TREE_TRAVERSAL_DEPTH) {
      LOG_ERR("ACT", "Tree traversal depth limit reached at %s", basePath);
      clearTreeReadingSession();
      return false;
    }
    const size_t nextSlash = parentRelative.find('/', start);
    const std::string dirName =
        nextSlash == std::string::npos ? parentRelative.substr(start) : parentRelative.substr(start, nextSlash - start);
    const std::string dirEntry = dirName + "/";
    auto& parent = stack.back();
    const auto it = std::find(parent.entries.begin(), parent.entries.end(), dirEntry);
    if (it == parent.entries.end()) return false;
    parent.nextEntry = static_cast<size_t>(it - parent.entries.begin()) + 1;

    DirState child{joinPath(parent.path, dirName), {}, 0};
    if (!loadTreeEntries(child.path, child.entries)) return false;
    stack.push_back(std::move(child));
    if (nextSlash == std::string::npos) break;
    start = nextSlash + 1;
  }

  auto& leaf = stack.back();
  const auto currentIt = std::find(leaf.entries.begin(), leaf.entries.end(), targetFile);
  if (currentIt == leaf.entries.end()) return false;
  leaf.nextEntry = static_cast<size_t>(currentIt - leaf.entries.begin()) + 1;

  while (!stack.empty()) {
    auto& state = stack.back();
    if (state.nextEntry >= state.entries.size()) {
      stack.pop_back();
      continue;
    }

    const std::string entry = state.entries[state.nextEntry++];
    if (entry.empty()) continue;
    if (entry.back() == '/') {
      if (stack.size() >= MAX_TREE_TRAVERSAL_DEPTH) {
        LOG_ERR("ACT", "Tree traversal depth limit reached while scanning %s", state.path.c_str());
        clearTreeReadingSession();
        return false;
      }
      DirState child{joinPath(state.path, entry.substr(0, entry.size() - 1)), {}, 0};
      if (loadTreeEntries(child.path, child.entries)) {
        stack.push_back(std::move(child));
      }
      continue;
    }

    goToReader(joinPath(state.path, entry), false, true);
    return true;
  }

  return false;
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, bool cleanInitialRefresh) {
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "Library") {
      initialMenuItem = HomeMenuItem::LIBRARY;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem, cleanInitialRefresh));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  mappedInput.resetHomeButtonInput();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  mappedInput.resetHomeButtonInput();
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock(Mode mode) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, mode == Mode::Try ? 0 : portMAX_DELAY) == pdTRUE;
  assert((mode == Mode::Try || isLocked) && "Blocking render lock acquisition failed");
}

RenderLock::RenderLock(Activity&) : RenderLock(Mode::Blocking) {}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };

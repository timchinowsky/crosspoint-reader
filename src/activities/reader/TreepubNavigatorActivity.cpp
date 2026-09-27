#include "TreepubNavigatorActivity.h"

#include <I18n.h>

#include <algorithm>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

TreepubNavigatorActivity::TreepubNavigatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::shared_ptr<Treepub> treepub, const uint32_t currentNodeId,
                                                   std::vector<uint32_t> history)
    : UiListActivity("TreepubNavigator", renderer, mappedInput),
      treepub(std::move(treepub)),
      currentNodeId(currentNodeId),
      history(std::move(history)) {}

void TreepubNavigatorActivity::rebuildRows() {
  rows.clear();
  rowLabels.clear();
  rowItems.clear();
  if (!treepub) return;

  const Treepub::Node* current = treepub->getNode(currentNodeId);
  if (!current) return;

  if (const auto parentId = treepub->getParent(currentNodeId); parentId.has_value()) {
    if (const Treepub::Node* parent = treepub->getNode(parentId.value())) {
      rows.push_back(Row{parent->id, std::string("^ ") + parent->title});
    }
  }

  if (const auto parentId = treepub->getParent(currentNodeId); parentId.has_value()) {
    if (const Treepub::Node* parent = treepub->getNode(parentId.value())) {
      for (uint32_t siblingId : parent->children) {
        const Treepub::Node* sibling = treepub->getNode(siblingId);
        if (!sibling) continue;
        const std::string prefix = siblingId == currentNodeId ? "* " : "  ";
        rows.push_back(Row{siblingId, prefix + sibling->title});
      }
    }
  } else {
    rows.push_back(Row{current->id, std::string("* ") + current->title});
  }

  for (uint32_t childId : current->children) {
    const Treepub::Node* child = treepub->getNode(childId);
    if (!child) continue;
    rows.push_back(Row{child->id, std::string("> ") + child->title});
  }

  for (auto it = history.rbegin(); it != history.rend() && rows.size() < 32; ++it) {
    if (*it == currentNodeId) continue;
    const Treepub::Node* node = treepub->getNode(*it);
    if (!node) continue;
    rows.push_back(Row{node->id, std::string("# ") + node->title});
  }

  rowLabels.reserve(rows.size());
  rowItems.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); i++) {
    rowLabels.push_back(rows[i].label);
    fui::ListItem item;
    item.label = rowLabels.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    rowItems.push_back(item);
  }
}

void TreepubNavigatorActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
  for (int i = 0; i < static_cast<int>(rows.size()); i++) {
    if (rows[i].nodeId == currentNodeId) {
      nav.selected = i;
      break;
    }
  }
}

bool TreepubNavigatorActivity::handleButtons() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  return UiListActivity::handleButtons();
}

void TreepubNavigatorActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  app.clearTapFlash();
  setResult(Result{rows[index].nodeId});
  finish();
}

void TreepubNavigatorActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (rows.empty()) {
    screen.centeredText(tr(STR_EMPTY_FILE), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  props.items = rowItems.data();
  props.itemsWindowFirst = 0;
  screen.list(props);
}

void TreepubNavigatorActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_TREEPUB_NAVIGATOR));
}

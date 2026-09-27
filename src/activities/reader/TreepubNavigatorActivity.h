#pragma once

#include <Treepub.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/UiAppHost.h"

class TreepubNavigatorActivity final : public UiListActivity {
 public:
  struct Row {
    uint32_t nodeId = 0;
    std::string label;
  };

  struct Result {
    uint32_t nodeId = 0;
  };

 private:
  std::shared_ptr<Treepub> treepub;
  uint32_t currentNodeId = 0;
  std::vector<uint32_t> history;
  std::vector<Row> rows;
  std::vector<std::string> rowLabels;
  std::vector<freeink::ui::ListItem> rowItems;

  void rebuildRows();

  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;

 public:
  TreepubNavigatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Treepub> treepub,
                           uint32_t currentNodeId, std::vector<uint32_t> history);
  void onEnter() override;
};

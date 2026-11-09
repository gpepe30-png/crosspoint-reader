#pragma once

#include <array>

#include "activities/UiListActivity.h"

class AppsActivity final : public UiListActivity {
  std::array<freeink::ui::ListItem, 3> rowItems_{};

  int listCount() const override { return static_cast<int>(rowItems_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

 public:
  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
};
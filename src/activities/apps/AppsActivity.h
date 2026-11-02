#pragma once

#include "activities/UiListActivity.h"

class AppsActivity final : public UiListActivity {
  int listCount() const override { return 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  void navigateButtons() override {}
  const char* headerTitle() const override;
  void drawFooter() override;

 public:
  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
};
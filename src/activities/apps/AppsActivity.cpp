#include "AppsActivity.h"

#include <I18n.h>

#include "components/UITheme.h"

AppsActivity::AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Apps", renderer, mappedInput) {}

const char* AppsActivity::headerTitle() const { return tr(STR_APPS); }

void AppsActivity::buildScreen(UiScreen& screen) { screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText); }

void AppsActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
#include "AppsActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

AppsActivity::AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Apps", renderer, mappedInput) {}

void AppsActivity::onEnter() {
  rowItems_[0].label = tr(STR_TRIVIA);
  rowItems_[0].actionValue = 0;
  UiListActivity::onEnter();
}

const char* AppsActivity::headerTitle() const { return tr(STR_APPS); }

void AppsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(freeink::ui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight),
                                                       0, static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  freeink::ui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = freeink::ui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void AppsActivity::activateIndex(const int index) {
  if (index != 0) return;
  app.clearTapFlash();
  activityManager.goToTrivia();
}
#include "DailyReadingsActivity.h"

#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "MappedInputManager.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"

DailyReadingsActivity::DailyReadingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Daily Readings", renderer, mappedInput) {}

void DailyReadingsActivity::onEnter() {
  windowValid_ = usccb::makeWindow(std::time(nullptr), days_.data());
  todayKey_ = windowValid_ ? days_[3].year * 10000 + days_[3].month * 100 + days_[3].day : -1;
  refreshRows();
  UiListActivity::onEnter();
}

void DailyReadingsActivity::onExit() {
  releaseWifi();
  UiListActivity::onExit();
}

void DailyReadingsActivity::loop() {
  usccb::Day currentWindow[usccb::kWindowSize];
  if (usccb::makeWindow(std::time(nullptr), currentWindow)) {
    const int currentKey = currentWindow[3].year * 10000 + currentWindow[3].month * 100 + currentWindow[3].day;
    if (currentKey != todayKey_) {
      std::copy(std::begin(currentWindow), std::end(currentWindow), days_.begin());
      todayKey_ = currentKey;
      windowValid_ = true;
      results_.fill(usccb::FetchResult::Saved);
      refreshRows();
      requestUpdate();
    }
  }
  UiListActivity::loop();
}

const char* DailyReadingsActivity::headerTitle() const { return tr(STR_DAILY_READINGS); }

void DailyReadingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(freeink::ui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight),
                                                       0, static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  freeink::ui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = freeink::ui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void DailyReadingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index == kTodayRow) {
    if (cached_[kTodayIndex]) {
      openReading(kTodayIndex);
    } else {
      beginFetch(FetchPlan::Today);
    }
    return;
  }
  if (index == kSelectDayRow) {
    selectDayExpanded_ = !selectDayExpanded_;
    refreshRows();
    requestUpdate();
    return;
  }
  if (index >= kFirstCachedDayRow && index < visibleRowCount_) {
    openReading(rowItems_[index].actionValue);
  }
}

bool DailyReadingsActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, kSelectDayHoldMs) &&
      nav.selected == kSelectDayRow) {
    beginFetch(FetchPlan::Week);
    return true;
  }
  return UiListActivity::handleButtons();
}

void DailyReadingsActivity::refreshRows() {
  for (size_t i = 0; i < usccb::kWindowSize; ++i) {
    if (windowValid_) {
      std::snprintf(dayLabels_[i].data(), dayLabels_[i].size(), "%04d-%02d-%02d", days_[i].year, days_[i].month,
                    days_[i].day);
      cached_[i] = usccb::isCached(days_[i]);
    } else {
      std::snprintf(dayLabels_[i].data(), dayLabels_[i].size(), "---- -- --");
      cached_[i] = false;
    }
  }

    rowItems_[kTodayRow].label = tr(STR_DAILY_READINGS_TODAY);
    rowItems_[kTodayRow].value = cached_[kTodayIndex]
                 ? tr(STR_DAILY_READINGS_CACHED)
                 : results_[kTodayIndex] == usccb::FetchResult::NotPublished
                   ? tr(STR_DAILY_READINGS_NOT_PUBLISHED)
                   : tr(STR_DAILY_READINGS_MISSING);
    rowItems_[kTodayRow].enabled = windowValid_ && !updating_;
  rowItems_[kTodayRow].actionValue = kTodayRow;

  rowItems_[kSelectDayRow].label = tr(STR_DAILY_READINGS_SELECT_DAY);
  rowItems_[kSelectDayRow].value = nullptr;
    rowItems_[kSelectDayRow].subtitle = status_[0] != '\0' ? status_ : nullptr;
    rowItems_[kSelectDayRow].enabled = windowValid_ && !updating_;
  rowItems_[kSelectDayRow].toggle = true;
  rowItems_[kSelectDayRow].toggleChecked = selectDayExpanded_;
  rowItems_[kSelectDayRow].actionValue = kSelectDayRow;

  visibleRowCount_ = kFirstCachedDayRow;
  if (!selectDayExpanded_) return;

  for (size_t i = 0; i < usccb::kWindowSize; ++i) {
    if (i == kTodayIndex || !cached_[i]) continue;
    auto& row = rowItems_[visibleRowCount_++];
    row.label = dayLabels_[i].data();
    row.value = tr(STR_DAILY_READINGS_CACHED);
    row.enabled = true;
    row.actionValue = static_cast<int16_t>(i);
    row.toggle = false;
    row.toggleChecked = false;
  }

  if (visibleRowCount_ == kFirstCachedDayRow) {
    auto& row = rowItems_[visibleRowCount_++];
    row.label = tr(STR_DAILY_READINGS_NO_CACHED_DAYS);
    row.value = nullptr;
    row.enabled = false;
    row.actionValue = 0;
    row.toggle = false;
    row.toggleChecked = false;
  }
}

void DailyReadingsActivity::beginFetch(const FetchPlan plan) {
  if (updating_) return;
  if (!windowValid_) {
    std::snprintf(status_, sizeof(status_), "%s", tr(STR_DAILY_READINGS_SET_CLOCK));
    refreshRows();
    requestUpdate();
    return;
  }
  pendingFetch_ = plan;
  updating_ = true;
  std::snprintf(status_, sizeof(status_), tr(STR_DAILY_READINGS_UPDATING),
                plan == FetchPlan::Today ? kTodayIndex + 1 : 0);
  refreshRows();
  requestUpdate();
  if (WiFi.status() == WL_CONNECTED) {
    runPendingFetch();
    return;
  }

  WiFi.mode(WIFI_STA);
  wifiActivated_ = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             updating_ = false;
                             std::snprintf(status_, sizeof(status_), "%s", tr(STR_DAILY_READINGS_UPDATE_FAILED));
                             releaseWifi();
                             refreshRows();
                             requestUpdate();
                             return;
                           }
                           runPendingFetch();
                         });
}

void DailyReadingsActivity::runPendingFetch() {
  switch (pendingFetch_) {
    case FetchPlan::Today:
      fetchToday();
      break;
    case FetchPlan::Week:
      updateWeek();
      break;
  }
}

void DailyReadingsActivity::fetchToday() {
  results_[kTodayIndex] = usccb::fetchDay(days_[kTodayIndex]);
  updating_ = false;
  std::snprintf(status_, sizeof(status_), "%s",
                results_[kTodayIndex] == usccb::FetchResult::Saved ? tr(STR_DAILY_READINGS_UPDATE_DONE)
                                                                    : tr(STR_DAILY_READINGS_UPDATE_FAILED));
  refreshRows();
  releaseWifi();
  requestUpdate();
}

void DailyReadingsActivity::updateWeek() {
  constexpr uint32_t kRequestSpacingMs = 4000;
  bool failed = !usccb::pruneExpiredCache(days_[kTodayIndex]);
  bool requestedDay = false;
  for (size_t i = 0; i < usccb::kWindowSize; ++i) {
    if (usccb::isCached(days_[i])) continue;
    std::snprintf(status_, sizeof(status_), tr(STR_DAILY_READINGS_UPDATING), static_cast<int>(i + 1));
    refreshRows();
    requestUpdateAndWait();
    if (requestedDay) delay(kRequestSpacingMs);
    results_[i] = usccb::fetchDay(days_[i]);
    requestedDay = true;
    if (results_[i] == usccb::FetchResult::Failed) {
      failed = true;
      break;
    }
  }
  updating_ = false;
  std::snprintf(status_, sizeof(status_), "%s",
                failed ? tr(STR_DAILY_READINGS_UPDATE_FAILED)
                       : tr(STR_DAILY_READINGS_UPDATE_DONE));
  refreshRows();
  releaseWifi();
  requestUpdate();
}

void DailyReadingsActivity::releaseWifi() {
  if (!wifiActivated_) return;
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }
  wifiActivated_ = false;
}

void DailyReadingsActivity::openReading(const int index) {
  if (!cached_[index]) return;
  const std::string path = usccb::cachePath(days_[index]);
  auto reader = ReaderActivity::create(renderer, mappedInput, path, false);
  if (!reader) {
    std::snprintf(status_, sizeof(status_), "%s", tr(STR_DAILY_READINGS_UPDATE_FAILED));
    refreshRows();
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(reader), [this](const ActivityResult&) { requestUpdate(); });
}
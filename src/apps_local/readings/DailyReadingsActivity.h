#pragma once

#include <array>

#include "activities/UiListActivity.h"
#include "UsccbReadings.h"

class DailyReadingsActivity final : public UiListActivity {
 public:
  DailyReadingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
    enum class FetchPlan { Today, Week };

    static constexpr int kTodayIndex = 3;
    static constexpr int kTodayRow = 0;
    static constexpr int kSelectDayRow = 1;
    static constexpr int kFirstCachedDayRow = 2;
    static constexpr unsigned long kSelectDayHoldMs = 1000;

    int listCount() const override { return visibleRowCount_; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
    bool handleButtons() override;
  const char* headerTitle() const override;

  void refreshRows();
    void beginFetch(FetchPlan plan);
    void runPendingFetch();
    void fetchToday();
  void updateWeek();
  void releaseWifi();
  void openReading(int index);

  std::array<usccb::Day, usccb::kWindowSize> days_{};
  std::array<std::array<char, 20>, usccb::kWindowSize> dayLabels_{};
  std::array<usccb::FetchResult, usccb::kWindowSize> results_{};
  std::array<bool, usccb::kWindowSize> cached_{};
    std::array<freeink::ui::ListItem, kFirstCachedDayRow + usccb::kWindowSize> rowItems_{};
  char status_[64] = "";
  int todayKey_ = -1;
  int visibleRowCount_ = kFirstCachedDayRow;
    FetchPlan pendingFetch_ = FetchPlan::Today;
  bool windowValid_ = false;
  bool wifiActivated_ = false;
  bool updating_ = false;
  bool selectDayExpanded_ = false;
};
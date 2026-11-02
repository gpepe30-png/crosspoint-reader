#pragma once

#include "activities/Activity.h"
#include "trivia/TriviaPackStore.h"

struct Rect;

class TriviaPackActivity final : public Activity {
  enum class State {
    NoPack,
    Ready,
    ModeSelect,
    Settings,
    Quizmaster,
    Solo,
    RoundOver,
    Reason,
    ConnectingWifi,
    Syncing,
    SyncResult,
    SyncFailed,
    UpdateAvailable,
    Downloading,
    Failed
  };

  TriviaPackStore store_;
  State state_ = State::NoPack;
  bool soloSelected_ = false;
  bool allowUsCentric_ = false;
  bool haveQuestion_ = false;
  bool answerRevealed_ = false;
  bool reportQueued_ = false;
  bool reportActionFailed_ = false;
  int difficulty_ = 0;
  int settingsRow_ = 0;
  int selectedOption_ = 0;
  int lastBottomChoice_ = -1;
  int chosenOption_ = -1;
  int reasonSelection_ = 0;
  State reportReturnState_ = State::Quizmaster;
  uint32_t currentQuestionIndex_ = 0;
  bool startSync_ = false;
  bool cancelDownload_ = false;
  bool leaveAfterCancel_ = false;
  bool downloadFailed_ = false;
  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;
  bool syncReportFailed_ = false;
  char status_[160] = {};
  trivia::PackManifest latestManifest_;
  trivia::Question question_;
  trivia::Choices choices_;
  trivia::Rng rng_{1};
  trivia::Chooser chooser_;
  trivia::Score score_;

  void onWifiComplete(bool connected);
  void beginWifiSelection();
  void syncNow();
  void downloadPack(const trivia::PackManifest& manifest);
  void loadPreferences();
  bool savePreferences(int difficulty, bool allowUsCentric);
  void dealQuestion();
  void beginQuestionReport();
  void finishQuestionReport(bool undo);
  void startRound(State mode);
  void drawQuestion(Rect content);

 public:
  TriviaPackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
};

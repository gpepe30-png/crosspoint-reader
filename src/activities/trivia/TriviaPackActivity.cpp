#include "TriviaPackActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <utility>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "trivia/TriviaSyncClient.h"

namespace {

constexpr const char* kPackUrl = "https://github.com/ma-r-s/crossplay/releases/download/trivia-pack/pack.dat";
constexpr const char* kPackPartPath = "/trivia/pack.dat.part";
constexpr const char* kPreferencesPath = "/trivia/prefs";
constexpr unsigned long kProgressUpdateMs = 1000;
constexpr unsigned long kReportHoldMs = 900;

Rect contentBounds(GfxRenderer& renderer, UITheme& theme) {
  const auto metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, true, false);
  const int top = safe.y + metrics.topPadding + metrics.headerHeight;
  const int bottom = safe.y + safe.height - metrics.buttonHintsHeight;
  return Rect{safe.x + metrics.contentSidePadding, top, safe.width - metrics.contentSidePadding * 2, bottom - top};
}

void drawOutlinedOption(const GfxRenderer& renderer, const Rect bounds, const char* text, const bool selected,
                        const bool correct) {
  if (correct) {
    renderer.fillRect(bounds.x, bounds.y, bounds.width, bounds.height, true);
  } else if (selected) {
    renderer.drawRect(bounds.x, bounds.y, bounds.width, bounds.height, 2, true);
  } else {
    renderer.drawRect(bounds.x, bounds.y, bounds.width, bounds.height, true);
  }
  UITheme::drawCenteredWrappedText(renderer, bounds, UI_10_FONT_ID, text, 2, !correct,
                                   EpdFontFamily::REGULAR);
}

const char* reportReasonLabel(const int selection) {
  switch (static_cast<trivia::Reason>(selection)) {
    case trivia::Reason::None:
      return tr(STR_TRIVIA_REASON_NONE);
    case trivia::Reason::Wrong:
      return tr(STR_TRIVIA_REASON_WRONG);
    case trivia::Reason::Nonsense:
      return tr(STR_TRIVIA_REASON_NONSENSE);
    case trivia::Reason::Giveaway:
      return tr(STR_TRIVIA_REASON_GIVEAWAY);
    case trivia::Reason::Ambiguous:
      return tr(STR_TRIVIA_REASON_AMBIGUOUS);
    case trivia::Reason::Outdated:
      return tr(STR_TRIVIA_REASON_OUTDATED);
    case trivia::Reason::Broken:
      return tr(STR_TRIVIA_REASON_BROKEN);
    case trivia::Reason::Regional:
      return tr(STR_TRIVIA_REASON_REGIONAL);
    case trivia::Reason::Us:
      return tr(STR_TRIVIA_REASON_US);
    case trivia::Reason::Hard:
      return tr(STR_TRIVIA_REASON_HARD);
    case trivia::Reason::Easy:
      return tr(STR_TRIVIA_REASON_EASY);
    case trivia::Reason::Count:
      break;
  }
  return tr(STR_TRIVIA_REASON_NONE);
}

}  // namespace

TriviaPackActivity::TriviaPackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("TriviaPack", renderer, mappedInput) {}

void TriviaPackActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    if (store_.open()) {
      state_ = State::Ready;
      chooser_.begin(store_.questionPack(), store_.questionState(), rng_);
      rng_.seed(static_cast<uint32_t>(millis()) | 1u);
      loadPreferences();
    } else {
      state_ = Storage.exists("/trivia/pack.dat") ? State::Failed : State::NoPack;
    }
  }
  requestUpdate();
}

void TriviaPackActivity::onExit() {
  store_.close();
  if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.disconnect(false);
  Activity::onExit();
}

void TriviaPackActivity::onWifiComplete(const bool connected) {
  if (!connected) {
    {
      RenderLock lock(*this);
      state_ = store_.hasPack() ? State::Ready : (Storage.exists("/trivia/pack.dat") ? State::Failed : State::NoPack);
    }
    requestUpdate(true);
    return;
  }
  startSync_ = true;
}

void TriviaPackActivity::loadPreferences() {
  HalFile file = Storage.open(kPreferencesPath, O_RDONLY);
  if (!file.isOpen()) return;

  uint8_t preferences[2] = {};
  const int bytesRead = file.read(preferences, sizeof(preferences));
  if (!file.close()) {
    LOG_ERR("TRIVIA", "Cannot close preferences file");
    return;
  }
  if (bytesRead < 1) {
    LOG_ERR("TRIVIA", "Cannot read trivia preferences");
    return;
  }
  if (preferences[0] <= trivia::kDifficulties) {
    difficulty_ = preferences[0];
  } else {
    LOG_ERR("TRIVIA", "Invalid saved difficulty; using any difficulty");
  }
  if (bytesRead >= 2 && preferences[1] <= 1) {
    allowUsCentric_ = preferences[1] != 0;
  } else if (bytesRead >= 2) {
    LOG_ERR("TRIVIA", "Invalid saved US-centric preference; using disabled");
  }
}

bool TriviaPackActivity::savePreferences(const int difficulty, const bool allowUsCentric) {
  HalFile file;
  if (!Storage.openFileForWrite("TRIVIA", kPreferencesPath, file)) {
    LOG_ERR("TRIVIA", "Cannot write trivia preferences");
    return false;
  }

  const uint8_t preferences[] = {static_cast<uint8_t>(difficulty), static_cast<uint8_t>(allowUsCentric)};
  if (file.write(preferences, sizeof(preferences)) != sizeof(preferences)) {
    LOG_ERR("TRIVIA", "Short write saving trivia preferences");
    if (!file.close()) LOG_ERR("TRIVIA", "Cannot close trivia preferences");
    return false;
  }
  file.flush();
  if (!file.close()) {
    LOG_ERR("TRIVIA", "Cannot close trivia preferences");
    return false;
  }
  return true;
}

void TriviaPackActivity::beginWifiSelection() {
  {
    RenderLock lock(*this);
    state_ = State::ConnectingWifi;
  }
  requestUpdate();
  auto wifiActivity = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifiActivity) {
    LOG_ERR("TRIVIA", "OOM: Wi-Fi selection activity");
    {
      RenderLock lock(*this);
      state_ = store_.hasPack() ? State::Ready : State::Failed;
    }
    requestUpdate(true);
    return;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::move(wifiActivity),
                         [this](const ActivityResult& result) { onWifiComplete(!result.isCancelled); });
}

void TriviaPackActivity::loop() {
  State currentState;
  bool haveQuestion;
  int chosenOption;
  {
    RenderLock lock(*this);
    currentState = state_;
    haveQuestion = haveQuestion_;
    chosenOption = chosenOption_;
  }

  if ((currentState == State::Quizmaster || currentState == State::Solo) && haveQuestion &&
      mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, kReportHoldMs)) {
    {
      RenderLock lock(*this);
      beginQuestionReport();
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && currentState != State::Downloading) {
    switch (currentState) {
      case State::ModeSelect:
        {
          RenderLock lock(*this);
          state_ = State::Ready;
        }
        requestUpdate();
        return;
      case State::Settings:
        {
          RenderLock lock(*this);
          state_ = State::Ready;
        }
        requestUpdate();
        return;
      case State::Reason:
        {
          RenderLock lock(*this);
          finishQuestionReport(true);
        }
        requestUpdate();
        return;
      case State::SyncResult:
      case State::SyncFailed:
      case State::UpdateAvailable:
        {
          RenderLock lock(*this);
          state_ = store_.hasPack() ? State::Ready : State::NoPack;
        }
        requestUpdate();
        return;
      case State::Quizmaster:
        {
          RenderLock lock(*this);
          state_ = State::ModeSelect;
        }
        requestUpdate();
        return;
      case State::Solo:
        {
          RenderLock lock(*this);
          state_ = State::RoundOver;
        }
        requestUpdate();
        return;
      case State::RoundOver:
        {
          RenderLock lock(*this);
          state_ = State::ModeSelect;
        }
        requestUpdate();
        return;
      case State::NoPack:
      case State::Ready:
      case State::ConnectingWifi:
      case State::Syncing:
      case State::Downloading:
      case State::Failed:
        finish();
        return;
    }
  }

  const bool confirmed = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmed && (currentState == State::NoPack || currentState == State::Failed)) {
    beginWifiSelection();
    return;
  }

  if (confirmed && currentState == State::Ready) {
    {
      RenderLock lock(*this);
      state_ = State::ModeSelect;
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext) && currentState == State::Ready) {
    beginWifiSelection();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) && currentState == State::Ready) {
    {
      RenderLock lock(*this);
      settingsRow_ = 0;
      state_ = State::Settings;
    }
    requestUpdate();
    return;
  }

  if (currentState == State::Settings) {
    const bool movePrevious = mappedInput.wasReleased(MappedInputManager::Button::NavPrevious);
    const bool moveNext = mappedInput.wasReleased(MappedInputManager::Button::NavNext);
    if (movePrevious || moveNext) {
      {
        RenderLock lock(*this);
        settingsRow_ = (settingsRow_ + 1) % 3;
      }
      requestUpdate();
      return;
    }
    if (confirmed) {
      if (settingsRow_ == 2) {
        beginWifiSelection();
        return;
      }
      bool saved = false;
      int difficulty;
      bool allowUsCentric;
      {
        RenderLock lock(*this);
        if (settingsRow_ == 0) {
          difficulty_ = (difficulty_ + 1) % (trivia::kDifficulties + 1);
        } else {
          allowUsCentric_ = !allowUsCentric_;
        }
        difficulty = difficulty_;
        allowUsCentric = allowUsCentric_;
      }
      saved = savePreferences(difficulty, allowUsCentric);
      if (!saved) LOG_ERR("TRIVIA", "Preference change is active but could not be saved");
      requestUpdate();
      return;
    }
  }

  if (currentState == State::Reason) {
    const bool movePrevious = mappedInput.wasReleased(MappedInputManager::Button::NavPrevious);
    const bool moveNext = mappedInput.wasReleased(MappedInputManager::Button::NavNext);
    if (movePrevious || moveNext) {
      {
        RenderLock lock(*this);
        reasonSelection_ =
            (reasonSelection_ + (movePrevious ? static_cast<int>(trivia::Reason::Count) - 1 : 1)) %
            static_cast<int>(trivia::Reason::Count);
      }
      requestUpdate();
      return;
    }
    if (confirmed) {
      {
        RenderLock lock(*this);
        finishQuestionReport(false);
      }
      requestUpdate();
      return;
    }
  }

  if (currentState == State::ModeSelect) {
    if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious) ||
        mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
      {
        RenderLock lock(*this);
        soloSelected_ = !soloSelected_;
      }
      requestUpdate();
      return;
    }
    if (confirmed) {
      startRound(soloSelected_ ? State::Solo : State::Quizmaster);
      return;
    }
  }

  if (currentState == State::Quizmaster) {
    if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) {
      {
        RenderLock lock(*this);
        dealQuestion();
      }
      requestUpdate();
      return;
    }
    if (confirmed) {
      {
        RenderLock lock(*this);
        if (!haveQuestion_ || answerRevealed_) {
          dealQuestion();
        } else {
          answerRevealed_ = true;
        }
      }
      requestUpdate();
      return;
    }
  }

  if (currentState == State::Solo) {
    const bool movePrevious = mappedInput.wasReleased(MappedInputManager::Button::NavPrevious);
    const bool moveNext = mappedInput.wasReleased(MappedInputManager::Button::NavNext);
    if (haveQuestion && chosenOption < 0 && (movePrevious || moveNext)) {
      {
        RenderLock lock(*this);
        if (movePrevious) {
          selectedOption_ = (selectedOption_ + trivia::kOptions - 1) % trivia::kOptions;
        } else {
          selectedOption_ = (selectedOption_ + 1) % trivia::kOptions;
        }
      }
      requestUpdate();
      return;
    }
    if (confirmed && !haveQuestion) {
      {
        RenderLock lock(*this);
        dealQuestion();
      }
      requestUpdate();
      return;
    }
    if (chosenOption >= 0 && (moveNext || confirmed)) {
      {
        RenderLock lock(*this);
        dealQuestion();
      }
      requestUpdate();
      return;
    }
    if (confirmed && haveQuestion && chosenOption < 0) {
      {
        RenderLock lock(*this);
        chosenOption_ = selectedOption_;
        score_.record(chosenOption_ == choices_.correct);
      }
      requestUpdate();
      return;
    }
  }

  if (currentState == State::RoundOver && confirmed) {
    {
      RenderLock lock(*this);
      state_ = State::ModeSelect;
    }
    requestUpdate();
    return;
  }

  if (currentState == State::UpdateAvailable && confirmed) {
    downloadPack(latestManifest_);
    return;
  }

  if (currentState == State::SyncResult && confirmed) {
    {
      RenderLock lock(*this);
      state_ = store_.hasPack() ? State::Ready : State::NoPack;
    }
    requestUpdate();
    return;
  }

  if (currentState == State::SyncFailed && confirmed) {
    beginWifiSelection();
    return;
  }

  if (startSync_) {
    startSync_ = false;
    syncNow();
    return;
  }

}

void TriviaPackActivity::startRound(const State mode) {
  {
    RenderLock lock(*this);
    state_ = mode;
    if (mode == State::Solo) score_.reset();
    dealQuestion();
  }
  requestUpdate();
}

void TriviaPackActivity::dealQuestion() {
  haveQuestion_ = false;
  answerRevealed_ = false;
  chosenOption_ = -1;
  selectedOption_ = 0;

  uint32_t index = 0;
  const bool solo = state_ == State::Solo;
  if (!chooser_.next(index, solo, difficulty_, allowUsCentric_)) {
    LOG_INF("TRIVIA", "No question available (difficulty %d, solo %d, US-centric %d)", difficulty_, solo ? 1 : 0,
            allowUsCentric_ ? 1 : 0);
    return;
  }
  if (!store_.questionPack().read(index, question_)) {
    LOG_ERR("TRIVIA", "Cannot read question %lu", static_cast<unsigned long>(index));
    return;
  }
  currentQuestionIndex_ = index;
  if (!store_.questionState().setFlag(index, trivia::kSeen)) {
    LOG_ERR("TRIVIA", "Cannot mark question %lu as seen", static_cast<unsigned long>(index));
    return;
  }
  if (solo && !trivia::buildChoices(question_, rng_, choices_)) {
    LOG_ERR("TRIVIA", "Cannot build answer choices for question %lu", static_cast<unsigned long>(index));
    return;
  }
  haveQuestion_ = true;
}

void TriviaPackActivity::beginQuestionReport() {
  reportQueued_ = false;
  reportActionFailed_ = false;
  reportReturnState_ = state_;
  if (!store_.questionState().setFlag(currentQuestionIndex_, trivia::kFlagged)) {
    LOG_ERR("TRIVIA", "Cannot hide question %lu", static_cast<unsigned long>(currentQuestionIndex_));
    reportActionFailed_ = true;
  } else {
    const trivia::QueueOpen queueState = store_.reportQueueState();
    if ((queueState == trivia::QueueOpen::Ready || queueState == trivia::QueueOpen::Started) &&
        store_.reportQueue().full() && store_.reportQueue().pending() == 0) {
      if (!store_.resetReportQueue()) LOG_ERR("TRIVIA", "Cannot make room in the report queue");
    }
    if (store_.reportQueueState() == trivia::QueueOpen::Ready ||
        store_.reportQueueState() == trivia::QueueOpen::Started) {
      reportQueued_ = store_.reportQueue().add(currentQuestionIndex_, trivia::Reason::None);
    }
    if (!reportQueued_) {
      LOG_ERR("TRIVIA", "Question hidden but report could not be queued");
      reportActionFailed_ = true;
    }
  }
  reasonSelection_ = 0;
  state_ = State::Reason;
}

void TriviaPackActivity::finishQuestionReport(const bool undo) {
  if (undo) {
    if (reportQueued_ && !store_.reportQueue().withdraw(currentQuestionIndex_)) {
      LOG_ERR("TRIVIA", "Cannot withdraw hidden-question report");
      reportActionFailed_ = true;
      return;
    }
    if (!store_.questionState().clearFlag(currentQuestionIndex_, trivia::kFlagged)) {
      LOG_ERR("TRIVIA", "Cannot unhide question %lu", static_cast<unsigned long>(currentQuestionIndex_));
      reportActionFailed_ = true;
      reportQueued_ = false;
      return;
    }
  } else if (reportQueued_ &&
             !store_.reportQueue().setReason(currentQuestionIndex_, static_cast<trivia::Reason>(reasonSelection_))) {
    LOG_ERR("TRIVIA", "Cannot save question report reason");
    reportActionFailed_ = true;
    return;
  }

  state_ = reportReturnState_;
  reportQueued_ = false;
  reportActionFailed_ = false;
}

void TriviaPackActivity::syncNow() {
  {
    RenderLock lock(*this);
    state_ = State::Syncing;
    syncReportFailed_ = false;
  }
  requestUpdateAndWait();

  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("TRIVIA", "Not enough heap to synchronize pack and reports");
    {
      RenderLock lock(*this);
      state_ = State::SyncFailed;
    }
    requestUpdate(true);
    return;
  }

  uint32_t pendingReports = 0;
  {
    RenderLock lock(*this);
    pendingReports = store_.reportQueue().pending();
  }
  if (pendingReports > 0) {
    auto body = makeUniqueNoThrow<char[]>(trivia::kMaxReportBatchBytes);
    if (!body) {
      LOG_ERR("TRIVIA", "OOM: report upload body");
      RenderLock lock(*this);
      syncReportFailed_ = true;
    } else {
      uint32_t reportCount = 0;
      uint32_t queueThrough = 0;
      size_t length = 0;
      {
        RenderLock lock(*this);
        auto& queue = store_.reportQueue();
        queueThrough = queue.count();
        length = trivia::formatReportBatch(queue, queueThrough, body.get(), trivia::kMaxReportBatchBytes, reportCount);
      }
      if (length == 0 || reportCount != pendingReports) {
        LOG_ERR("TRIVIA", "Cannot format pending report batch");
        RenderLock lock(*this);
        syncReportFailed_ = true;
      } else {
        const int status = trivia::submitReportBatch(body.get(), length);
        if (status != 202) {
          LOG_ERR("TRIVIA", "Report server returned status %d", status);
          RenderLock lock(*this);
          syncReportFailed_ = true;
        } else {
          RenderLock lock(*this);
          auto& queue = store_.reportQueue();
          if (!queue.markSent(queueThrough)) {
            LOG_ERR("TRIVIA", "Reports accepted but could not advance local queue");
            syncReportFailed_ = true;
          } else if (queue.pending() == 0 && !store_.resetReportQueue()) {
            LOG_ERR("TRIVIA", "Reports sent but queue could not be reset");
          }
        }
      }
    }
  } else {
    RenderLock lock(*this);
    if (store_.reportQueueState() == trivia::QueueOpen::Foreign && !store_.resetReportQueue())
      LOG_ERR("TRIVIA", "Cannot clear empty foreign report queue");
  }

  trivia::PackManifest manifest;
  if (!trivia::fetchPublishedManifest(manifest)) {
    {
      RenderLock lock(*this);
      state_ = State::SyncFailed;
    }
    requestUpdate(true);
    return;
  }
  trivia::Freshness freshness;
  {
    RenderLock lock(*this);
    latestManifest_ = manifest;
    if (!store_.packMeta().valid) store_.adoptManifest(manifest);
    freshness = trivia::compare(store_.packMeta(), manifest);
    state_ = freshness == trivia::Freshness::Current ? State::SyncResult : State::UpdateAvailable;
  }
  requestUpdate(true);
}

void TriviaPackActivity::downloadPack(const trivia::PackManifest& manifest) {
  cancelDownload_ = false;
  leaveAfterCancel_ = false;
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  downloadFailed_ = false;
  {
    RenderLock lock(*this);
    state_ = State::Downloading;
  }
  requestUpdateAndWait();

  if (!Storage.ensureDirectoryExists("/trivia")) {
    LOG_ERR("TRIVIA", "Cannot create trivia directory");
    {
      RenderLock lock(*this);
      state_ = State::Failed;
    }
    requestUpdate(true);
    return;
  }

  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("TRIVIA", "Not enough heap to start pack download");
    {
      RenderLock lock(*this);
      state_ = State::Failed;
    }
    requestUpdate(true);
    return;
  }

  unsigned long lastUpdate = 0;
  bool downloadRejected = false;
  const HttpDownloader::DownloadError result = HttpDownloader::downloadToFile(
      kPackUrl, kPackPartPath,
      [this, &lastUpdate, &manifest, &downloadRejected](const size_t downloaded, const size_t total) {
        if ((total > 0 && total != manifest.bytes) || downloaded > manifest.bytes) {
          downloadRejected = true;
          cancelDownload_ = true;
          return;
        }
        mappedInput.update(true);
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload_ = true;
        if (mappedInput.wasHomeGesture()) {
          cancelDownload_ = true;
          leaveAfterCancel_ = true;
        }
        const unsigned long now = millis();
        if (now - lastUpdate >= kProgressUpdateMs) {
          lastUpdate = now;
          {
            RenderLock lock(*this);
            downloadProgress_ = downloaded;
            downloadTotal_ = total;
          }
          requestUpdate(true);
        }
      },
      &cancelDownload_);

  if (result == HttpDownloader::OK) {
    RenderLock lock(*this);
    if (!store_.installDownloadedPack(manifest)) {
      const bool reopened = store_.open();
      if (!reopened) LOG_ERR("TRIVIA", "Cannot reopen question pack after failed install");
      downloadFailed_ = true;
      state_ = reopened ? State::Ready : State::Failed;
      if (reopened) chooser_.begin(store_.questionPack(), store_.questionState(), rng_);
    } else {
      state_ = State::Ready;
      chooser_.begin(store_.questionPack(), store_.questionState(), rng_);
    }
  } else {
    if (downloadRejected) {
      LOG_ERR("TRIVIA", "Downloaded pack size differs from the published manifest");
    } else if (result != HttpDownloader::ABORTED) {
      LOG_ERR("TRIVIA", "Pack download failed: %d", result);
    }
    RenderLock lock(*this);
    downloadFailed_ = result != HttpDownloader::ABORTED || downloadRejected;
    state_ = store_.hasPack() ? State::Ready : (downloadFailed_ ? State::Failed : State::NoPack);
  }

  if (leaveAfterCancel_) {
    onGoHome();
    return;
  }
  requestUpdate(true);
}

void TriviaPackActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, tr(STR_TRIVIA));

  const Rect content = contentBounds(renderer, theme);

  switch (state_) {
    case State::NoPack:
      snprintf(status_, sizeof(status_), "%s", tr(STR_TRIVIA_NO_PACK));
      break;
    case State::Ready:
      snprintf(status_, sizeof(status_), "%s: %lu\n%s: %lu%s", tr(STR_TRIVIA_PACK_READY),
               static_cast<unsigned long>(store_.questionCount()), tr(STR_TRIVIA_REPORTS_PENDING),
               static_cast<unsigned long>(store_.reportQueue().pending()),
               downloadFailed_ ? "\n" : "");
      if (downloadFailed_) {
        const size_t used = std::strlen(status_);
        snprintf(status_ + used, sizeof(status_) - used, "%s", tr(STR_TRIVIA_DOWNLOAD_FAILED));
      }
      break;
    case State::ModeSelect:
      snprintf(status_, sizeof(status_), "%s\n%s\n%s", tr(STR_TRIVIA_MODE),
               soloSelected_ ? tr(STR_TRIVIA_SOLO) : tr(STR_TRIVIA_QUIZMASTER),
               soloSelected_ ? tr(STR_TRIVIA_SOLO_HELP) : tr(STR_TRIVIA_QUIZMASTER_HELP));
      break;
    case State::Settings: {
      char difficultyValue[24] = {};
      const char* usCentricValue = allowUsCentric_ ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      if (difficulty_ == 0) {
        snprintf(difficultyValue, sizeof(difficultyValue), "%s", tr(STR_TRIVIA_ANY_DIFFICULTY));
      } else {
        snprintf(difficultyValue, sizeof(difficultyValue), "%u", static_cast<unsigned>(difficulty_));
      }
      snprintf(status_, sizeof(status_), "%s\n%s %s: %s\n%s %s: %s\n%s %s: %lu", tr(STR_TRIVIA_SETTINGS),
               settingsRow_ == 0 ? ">" : " ", tr(STR_TRIVIA_DIFFICULTY), difficultyValue,
               settingsRow_ == 1 ? ">" : " ", tr(STR_TRIVIA_US_CENTRIC), usCentricValue,
               settingsRow_ == 2 ? ">" : " ", tr(STR_TRIVIA_SYNC), static_cast<unsigned long>(store_.reportQueue().pending()));
      break;
    }
    case State::Quizmaster:
    case State::Solo:
      drawQuestion(content);
      break;
    case State::RoundOver:
      snprintf(status_, sizeof(status_), tr(STR_TRIVIA_SCORE), score_.right, score_.asked);
      break;
    case State::Reason:
      snprintf(status_, sizeof(status_), "%s\n%s %s\n%s", tr(STR_TRIVIA_REPORT_REASON),
               "> ", reportReasonLabel(reasonSelection_),
               reportActionFailed_ ? tr(STR_TRIVIA_REPORT_FAILED)
                                   : (reportQueued_ ? tr(STR_TRIVIA_REPORT_HIDDEN) : tr(STR_TRIVIA_REPORT_NOT_QUEUED)));
      break;
    case State::ConnectingWifi:
      snprintf(status_, sizeof(status_), "%s", tr(STR_TRIVIA_CONNECTING_WIFI));
      break;
    case State::Syncing:
      snprintf(status_, sizeof(status_), "%s", tr(STR_TRIVIA_SYNCING));
      break;
    case State::SyncResult:
      snprintf(status_, sizeof(status_), "%s\n%s", tr(STR_TRIVIA_PACK_CURRENT),
               syncReportFailed_ ? tr(STR_TRIVIA_REPORTS_RETAINED) : tr(STR_TRIVIA_SYNC_COMPLETE));
      break;
    case State::SyncFailed:
      if (syncReportFailed_) {
        snprintf(status_, sizeof(status_), "%s\n%s", tr(STR_TRIVIA_SYNC_FAILED),
                 tr(STR_TRIVIA_REPORTS_RETAINED));
      } else {
        snprintf(status_, sizeof(status_), "%s", tr(STR_TRIVIA_SYNC_FAILED));
      }
      break;
    case State::UpdateAvailable:
      snprintf(status_, sizeof(status_), "%s\n%s", tr(STR_TRIVIA_UPDATE_AVAILABLE),
               syncReportFailed_ ? tr(STR_TRIVIA_REPORTS_RETAINED) : tr(STR_TRIVIA_INSTALL_UPDATE));
      break;
    case State::Downloading:
      if (downloadTotal_ > 0) {
        const uint64_t percent = static_cast<uint64_t>(downloadProgress_) * 100 / downloadTotal_;
        snprintf(status_, sizeof(status_), "%s: %u%%", tr(STR_TRIVIA_DOWNLOADING), static_cast<unsigned>(percent));
      } else {
        snprintf(status_, sizeof(status_), "%s: %lu KB", tr(STR_TRIVIA_DOWNLOADING),
                 static_cast<unsigned long>(downloadProgress_ / 1024));
      }
      break;
    case State::Failed:
      snprintf(status_, sizeof(status_), "%s", tr(STR_TRIVIA_DOWNLOAD_FAILED));
      break;
  }
  if (state_ != State::Quizmaster && state_ != State::Solo) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, status_, 4, true, EpdFontFamily::REGULAR);
  }

  const char* backLabel = tr(STR_BACK);
  const char* confirmLabel = "";
  const char* previousLabel = "";
  const char* nextLabel = "";
  switch (state_) {
    case State::NoPack:
    case State::Failed:
      confirmLabel = tr(STR_DOWNLOAD);
      break;
    case State::Ready:
      confirmLabel = tr(STR_TRIVIA_PLAY);
      previousLabel = tr(STR_TRIVIA_SETTINGS);
      nextLabel = tr(STR_TRIVIA_CHECK_SYNC);
      break;
    case State::ModeSelect:
      confirmLabel = tr(STR_SELECT);
      previousLabel = tr(STR_TRIVIA_PREVIOUS);
      nextLabel = tr(STR_TRIVIA_NEXT);
      break;
    case State::Settings:
      confirmLabel = settingsRow_ == 2 ? tr(STR_TRIVIA_CHECK_SYNC) : tr(STR_TRIVIA_CHANGE);
      previousLabel = tr(STR_TRIVIA_PREVIOUS);
      nextLabel = tr(STR_TRIVIA_NEXT);
      break;
    case State::Quizmaster:
      confirmLabel = haveQuestion_ && !answerRevealed_ ? tr(STR_TRIVIA_REVEAL) : tr(STR_TRIVIA_NEXT);
      nextLabel = tr(STR_TRIVIA_NEXT);
      break;
    case State::Solo:
      confirmLabel = !haveQuestion_ ? tr(STR_RETRY) : (chosenOption_ < 0 ? tr(STR_SELECT) : tr(STR_TRIVIA_NEXT));
      previousLabel = haveQuestion_ && chosenOption_ < 0 ? tr(STR_TRIVIA_PREVIOUS) : "";
      nextLabel = haveQuestion_ ? tr(STR_TRIVIA_NEXT) : "";
      break;
    case State::RoundOver:
      confirmLabel = tr(STR_TRIVIA_PLAY);
      break;
    case State::Reason:
      backLabel = tr(STR_TRIVIA_UNDO_HIDE);
      confirmLabel = tr(STR_TRIVIA_CHANGE);
      previousLabel = tr(STR_TRIVIA_PREVIOUS);
      nextLabel = tr(STR_TRIVIA_NEXT);
      break;
    case State::SyncResult:
      confirmLabel = tr(STR_DONE);
      break;
    case State::SyncFailed:
      confirmLabel = tr(STR_RETRY);
      break;
    case State::UpdateAvailable:
      confirmLabel = tr(STR_DOWNLOAD);
      break;
    case State::ConnectingWifi:
    case State::Syncing:
    case State::Downloading:
      break;
  }
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void TriviaPackActivity::drawQuestion(const Rect content) {
  if (!haveQuestion_) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, tr(STR_TRIVIA_NO_QUESTIONS), 4, true,
                                     EpdFontFamily::REGULAR);
    return;
  }

  const bool solo = state_ == State::Solo;
  const int clueHeight = solo ? content.height * 2 / 5 : (answerRevealed_ ? content.height * 2 / 3 : content.height);
  const Rect clueBounds{content.x, content.y, content.width, clueHeight};
  UITheme::drawCenteredWrappedText(renderer, clueBounds, UI_12_FONT_ID, question_.clue(), solo ? 5 : 8, true,
                                   EpdFontFamily::REGULAR, UITheme::TextVerticalAlignment::TOP);
  if (!solo && answerRevealed_) {
    const Rect answerBounds{content.x, content.y + clueHeight, content.width, content.height - clueHeight};
    renderer.drawRect(answerBounds.x, answerBounds.y, answerBounds.width, answerBounds.height, true);
    UITheme::drawCenteredWrappedText(renderer, answerBounds, UI_12_FONT_ID, question_.answer(), 4, true,
                                     EpdFontFamily::BOLD);
    return;
  }
  if (!solo) return;

  const int optionsY = content.y + clueHeight;
  const int rowHeight = (content.height - clueHeight) / trivia::kOptions;
  for (int i = 0; i < trivia::kOptions; ++i) {
    const Rect optionBounds{content.x, optionsY + i * rowHeight, content.width,
                            i == trivia::kOptions - 1 ? content.y + content.height - (optionsY + i * rowHeight)
                                                      : rowHeight};
    const bool correct = chosenOption_ >= 0 && i == choices_.correct;
    const bool selected = chosenOption_ < 0 ? i == selectedOption_ : i == chosenOption_ && !correct;
    drawOutlinedOption(renderer, optionBounds, choices_.option[i], selected, correct);
  }
}

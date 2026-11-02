#pragma once

#include <HalStorage.h>

#include "TriviaCore.h"
#include "TriviaReport.h"

class TriviaPackStore {
 public:
  TriviaPackStore() = default;
  ~TriviaPackStore();
  TriviaPackStore(const TriviaPackStore&) = delete;
  TriviaPackStore& operator=(const TriviaPackStore&) = delete;

  bool open();
  bool installDownloadedPack(const trivia::PackManifest& manifest);
  bool adoptManifest(const trivia::PackManifest& manifest);
  bool resetReportQueue();
  bool close();
  bool hasPack() const { return pack_.isOpen(); }
  uint32_t questionCount() const { return pack_.count(); }
  uint32_t seenCount() const { return state_.seenCount(); }
  trivia::Pack& questionPack() { return pack_; }
  trivia::PackState& questionState() { return state_; }
  const trivia::PackMeta& packMeta() const { return packMeta_; }
  trivia::QueueOpen reportQueueState() const { return reportQueueState_; }
  trivia::ReportQueue& reportQueue() { return reports_; }

 private:
  class FileSource final : public trivia::WritableByteSource {
   public:
    void attach(HalFile& file, bool allowGrowth = false);
    void detach();
    bool read(uint32_t offset, void* dst, uint32_t length) override;
    bool write(uint32_t offset, const void* src, uint32_t length) override;
    bool flush() override;
    uint32_t size() const override { return size_; }

   private:
    HalFile* file_ = nullptr;
    uint32_t size_ = 0;
    bool allowGrowth_ = false;
  };

  bool recoverFiles();
  bool ensureState(uint32_t count);
  bool writeEmptyState(uint32_t count);
  bool loadPackMeta();
  bool writePackMeta(const trivia::PackManifest& manifest);
  trivia::QueueOpen openReportQueue();
  bool closeReportQueue();
  bool removeIfPresent(const char* path);
  bool restorePackAndState();
  bool restoreState();

  HalFile packFile_;
  HalFile stateFile_;
  HalFile reportFile_;
  FileSource packSource_;
  FileSource stateSource_;
  FileSource reportSource_;
  trivia::Pack pack_;
  trivia::PackState state_;
  trivia::ReportQueue reports_;
  trivia::PackMeta packMeta_;
  trivia::QueueOpen reportQueueState_ = trivia::QueueOpen::Unusable;
};

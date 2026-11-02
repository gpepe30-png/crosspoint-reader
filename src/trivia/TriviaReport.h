#pragma once

#include <cstddef>
#include <cstdint>

#include "TriviaCore.h"

namespace trivia {

inline constexpr uint8_t kReportMagic[8] = {'X', 'T', 'R', 'I', 'V', 'R', 'P', 'T'};
inline constexpr uint16_t kReportVersion = 1;
inline constexpr uint32_t kPackIdBytes = 32;
inline constexpr uint32_t kReportHeaderBytes = 52;
inline constexpr uint32_t kReportEntryBytes = 8;
inline constexpr uint32_t kMaxQueuedReports = 64;
inline constexpr size_t kMaxReportBatchBytes = 96 + kMaxQueuedReports * 48;
inline constexpr uint32_t kWithdrawnIndex = 0xFFFFFFFFu;

enum class Reason : uint8_t {
  None = 0,
  Wrong = 1,
  Nonsense = 2,
  Giveaway = 3,
  Ambiguous = 4,
  Outdated = 5,
  Broken = 6,
  Regional = 7,
  Us = 8,
  Hard = 9,
  Easy = 10,
  Count = 11,
};

const char* reasonName(Reason reason);

enum class QueueOpen : uint8_t { Ready, Started, Foreign, Unusable };

class ReportQueue {
 public:
  QueueOpen open(WritableByteSource& source, const char* packId, uint32_t packCount);

  bool isOpen() const { return source_ != nullptr; }
  uint32_t count() const { return count_; }
  uint32_t sent() const { return sent_; }
  uint32_t pending() const { return count_ - sent_ - withdrawnPending_; }
  uint32_t packCount() const { return packCount_; }
  const char* packId() const { return packId_; }
  bool full() const { return count_ >= kMaxQueuedReports; }

  bool add(uint32_t index, Reason reason);
  bool setReason(uint32_t index, Reason reason);
  bool entry(uint32_t slot, uint32_t& indexOut, Reason& reasonOut) const;
  bool withdraw(uint32_t index);
  bool markSent(uint32_t count);

 private:
  enum class FindResult : uint8_t { Found, Missing, ReadError };
  bool writeHeader();
  FindResult find(uint32_t index, uint32_t& slotOut) const;

  WritableByteSource* source_ = nullptr;
  uint32_t count_ = 0;
  uint32_t sent_ = 0;
  uint32_t packCount_ = 0;
  uint32_t withdrawnPending_ = 0;
  bool acceptsReports_ = false;
  char packId_[kPackIdBytes + 1] = {};
};

struct PackMeta {
  char id[kPackIdBytes + 1] = {};
  uint32_t count = 0;
  uint32_t bytes = 0;
  bool valid = false;
};

PackMeta parseMeta(const char* text, size_t length, uint32_t actualCount, uint32_t actualBytes);
size_t formatMeta(char* out, size_t capacity, const char* id, uint32_t count, uint32_t bytes);
size_t formatReportBatch(const ReportQueue& queue, uint32_t through, char* out, size_t capacity,
                         uint32_t& reportCount);

struct PackManifest {
  char id[kPackIdBytes + 1] = {};
  uint32_t count = 0;
  uint32_t bytes = 0;
  bool valid = false;
};

PackManifest parseManifest(const char* json, size_t length);

enum class Freshness : uint8_t { Current, Newer, Unknown, NoManifest };
Freshness compare(const PackMeta& held, const PackManifest& published);

}  // namespace trivia

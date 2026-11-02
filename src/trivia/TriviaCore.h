#pragma once

#include <cstddef>
#include <cstdint>

namespace trivia {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual bool read(uint32_t offset, void* dst, uint32_t length) = 0;
  virtual uint32_t size() const = 0;
};

class WritableByteSource : public ByteSource {
 public:
  virtual bool write(uint32_t offset, const void* src, uint32_t length) = 0;
  virtual bool flush() = 0;
};

inline constexpr uint32_t kMaxRecordBytes = 448;
inline constexpr int kMaxAlternates = 4;
inline constexpr int kMaxDistractors = 6;
inline constexpr int kOptions = 4;
inline constexpr int kDifficulties = 5;

inline constexpr uint8_t kMagic[8] = {'X', 'T', 'R', 'I', 'V', 'I', 'A', 0};
inline constexpr uint16_t kFormatVersion = 1;
inline constexpr uint8_t kUsCentric = 0x80;
inline constexpr uint8_t kDifficultyMask = 0x7F;
inline constexpr uint8_t kSeen = 0x01;
inline constexpr uint8_t kFlagged = 0x02;

class Question {
 public:
  uint8_t difficulty() const { return difficulty_; }
  bool usCentric() const { return usCentric_; }
  uint16_t year() const { return year_; }
  int alternateCount() const { return altCount_; }
  int distractorCount() const { return wrongCount_; }

  const char* clue() const { return field(0); }
  const char* answer() const { return field(1); }
  const char* alternate(int i) const { return (i >= 0 && i < altCount_) ? field(2 + i) : nullptr; }
  const char* distractor(int i) const { return (i >= 0 && i < wrongCount_) ? field(2 + altCount_ + i) : nullptr; }
  bool playableAsChoice() const { return wrongCount_ >= kOptions - 1; }

 private:
  friend class Pack;
  const char* field(int i) const { return (i >= 0 && i < fieldCount_) ? bytes_ + offset_[i] : ""; }

  uint8_t difficulty_ = 0;
  bool usCentric_ = false;
  uint16_t year_ = 0;
  uint8_t altCount_ = 0;
  uint8_t wrongCount_ = 0;
  int fieldCount_ = 0;
  uint16_t offset_[2 + kMaxAlternates + kMaxDistractors] = {};
  char bytes_[kMaxRecordBytes] = {};
};

class Pack {
 public:
  bool open(ByteSource& source);
  bool isOpen() const { return count_ > 0; }
  uint32_t count() const { return count_; }
  bool read(uint32_t index, Question& out) const;

 private:
  ByteSource* source_ = nullptr;
  uint32_t count_ = 0;
  uint32_t indexAt_ = 0;
  uint32_t base_ = 0;
  uint32_t blobBytes_ = 0;
};

class PackState {
 public:
  bool open(WritableByteSource& source, uint32_t count);
  bool flags(uint32_t index, uint8_t& result) const;
  bool setFlag(uint32_t index, uint8_t bit);
  bool clearFlag(uint32_t index, uint8_t bit);
  bool seen(uint32_t i) const {
    uint8_t value = 0;
    return flags(i, value) && (value & kSeen) != 0;
  }
  bool flagged(uint32_t i) const {
    uint8_t value = 0;
    return flags(i, value) && (value & kFlagged) != 0;
  }
  uint32_t seenCount() const { return seenCount_; }
  uint32_t flaggedCount() const { return flaggedCount_; }
  uint32_t count() const { return count_; }

 private:
  bool scanCounts();

  WritableByteSource* source_ = nullptr;
  uint32_t count_ = 0;
  uint32_t seenCount_ = 0;
  uint32_t flaggedCount_ = 0;
};

class Rng {
 public:
  explicit Rng(uint32_t seed = 1) : state_(seed ? seed : 1) {}
  uint32_t next();
  uint32_t below(uint32_t bound);
  void seed(uint32_t value) { state_ = value ? value : 1; }

 private:
  uint32_t state_;
};

class Chooser {
 public:
  void begin(const Pack& pack, PackState& state, Rng& rng);
  bool next(uint32_t& indexOut, bool requireChoice, int difficulty, bool allowUsCentric);

 private:
  const Pack* pack_ = nullptr;
  PackState* state_ = nullptr;
  Rng* rng_ = nullptr;
  Question candidate_;
};

struct Choices {
  const char* option[kOptions] = {};
  int correct = 0;
};

bool buildChoices(const Question& question, Rng& rng, Choices& out);
bool answerMatches(const Question& question, const char* given);

struct Score {
  int asked = 0;
  int right = 0;
  void record(bool correct) {
    ++asked;
    if (correct) ++right;
  }
  void reset() { asked = right = 0; }
};

enum class Room : uint8_t { Ok, Unknown, TooSmall };
constexpr uint64_t kPackFreeFloorBytes = 12ull * 1024 * 1024;
constexpr Room roomFor(bool queryOk, uint64_t freeBytes, uint64_t floorBytes) {
  if (!queryOk) return Room::Unknown;
  return freeBytes < floorBytes ? Room::TooSmall : Room::Ok;
}

enum class View : uint8_t { Menu, Quizmaster, Solo, Notice, Settings, Reason };
enum class Back : uint8_t { LeaveApp, EndRound, ToMenu, ToNotice };

constexpr Back backFrom(View view, bool packOpen) {
  switch (view) {
    case View::Menu:
      return Back::LeaveApp;
    case View::Quizmaster:
    case View::Solo:
      return Back::EndRound;
    case View::Notice:
      return packOpen ? Back::ToMenu : Back::LeaveApp;
    case View::Settings:
      return Back::ToMenu;
    case View::Reason:
      return Back::ToNotice;
  }
  return Back::ToMenu;
}

}  // namespace trivia

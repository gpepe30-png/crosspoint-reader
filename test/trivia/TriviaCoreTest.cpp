#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "src/trivia/TriviaCore.h"

namespace {

struct Record {
  uint8_t difficulty = 1;
  uint16_t year = 2020;
  uint8_t alternateCount = 0;
  uint8_t distractorCount = 0;
  bool usCentric = false;
  std::vector<std::string> fields;
};

void put16(std::vector<uint8_t>& output, uint16_t value) {
  output.push_back(static_cast<uint8_t>(value));
  output.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& output, uint32_t value) {
  for (int i = 0; i < 4; ++i) output.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

std::vector<uint8_t> buildPack(const std::vector<Record>& records) {
  std::vector<uint8_t> blob;
  std::vector<uint32_t> offsets;
  offsets.reserve(records.size() + 1);
  for (const Record& record : records) {
    offsets.push_back(static_cast<uint32_t>(blob.size()));
    uint8_t difficulty = record.difficulty;
    if (record.usCentric) difficulty |= trivia::kUsCentric;
    blob.push_back(difficulty);
    put16(blob, record.year);
    blob.push_back(record.alternateCount);
    blob.push_back(record.distractorCount);
    for (const std::string& field : record.fields) {
      put16(blob, static_cast<uint16_t>(field.size()));
      blob.insert(blob.end(), field.begin(), field.end());
    }
  }
  offsets.push_back(static_cast<uint32_t>(blob.size()));

  std::vector<uint8_t> packed;
  packed.insert(packed.end(), std::begin(trivia::kMagic), std::end(trivia::kMagic));
  put16(packed, trivia::kFormatVersion);
  put16(packed, 0);
  put32(packed, static_cast<uint32_t>(records.size()));
  for (const uint32_t offset : offsets) put32(packed, offset);
  packed.insert(packed.end(), blob.begin(), blob.end());
  return packed;
}

Record questionRecord(uint8_t difficulty, bool usCentric, bool withChoices) {
  Record record;
  record.difficulty = difficulty;
  record.usCentric = usCentric;
  record.fields = {"A clue", "Answer"};
  if (withChoices) {
    record.alternateCount = 1;
    record.distractorCount = 3;
    record.fields.insert(record.fields.end(), {"Alternate", "Wrong one", "Wrong two", "Wrong three"});
  }
  return record;
}

class MemorySource final : public trivia::WritableByteSource {
 public:
  explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  bool read(uint32_t offset, void* dst, uint32_t length) override {
    if (failReadOffset >= 0 && offset == static_cast<uint32_t>(failReadOffset)) return false;
    if (static_cast<uint64_t>(offset) + length > bytes_.size()) return false;
    std::memcpy(dst, bytes_.data() + offset, length);
    return true;
  }
  bool write(uint32_t offset, const void* src, uint32_t length) override {
    if (static_cast<uint64_t>(offset) + length > bytes_.size()) return false;
    std::memcpy(bytes_.data() + offset, src, length);
    ++writes;
    return true;
  }
  bool flush() override { return true; }
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }

  int writes = 0;
  int failReadOffset = -1;

 private:
  std::vector<uint8_t> bytes_;
};

TEST(TriviaCore, ReadsPackRecordsAndAlternateAnswers) {
  const Record record = questionRecord(4, true, true);
  MemorySource source(buildPack({record}));
  trivia::Pack pack;
  ASSERT_TRUE(pack.open(source));
  ASSERT_EQ(pack.count(), 1);

  trivia::Question question;
  ASSERT_TRUE(pack.read(0, question));
  EXPECT_STREQ(question.clue(), "A clue");
  EXPECT_STREQ(question.answer(), "Answer");
  EXPECT_STREQ(question.alternate(0), "Alternate");
  EXPECT_EQ(question.difficulty(), 4);
  EXPECT_EQ(question.year(), record.year);
  EXPECT_TRUE(question.usCentric());
  EXPECT_TRUE(question.playableAsChoice());
  EXPECT_TRUE(trivia::answerMatches(question, "aNSwer"));
  EXPECT_TRUE(trivia::answerMatches(question, "ALTERNATE"));
  EXPECT_FALSE(trivia::answerMatches(question, "not an answer"));
  EXPECT_FALSE(pack.read(1, question));
}

TEST(TriviaCore, RejectsInvalidAndTruncatedPackHeaders) {
  auto bytes = buildPack({questionRecord(1, false, false)});
  bytes[0] = 'Y';
  MemorySource badMagic(std::move(bytes));
  trivia::Pack pack;
  EXPECT_FALSE(pack.open(badMagic));

  bytes = buildPack({questionRecord(1, false, false)});
  bytes[8] = 99;
  MemorySource badVersion(std::move(bytes));
  EXPECT_FALSE(pack.open(badVersion));

  bytes = buildPack({questionRecord(1, false, false)});
  bytes.resize(20);
  MemorySource truncated(std::move(bytes));
  EXPECT_FALSE(pack.open(truncated));

  bytes.assign(16, 0);
  std::copy(std::begin(trivia::kMagic), std::end(trivia::kMagic), bytes.begin());
  bytes[8] = static_cast<uint8_t>(trivia::kFormatVersion);
  bytes[12] = 0xFF;
  bytes[13] = bytes[14] = bytes[15] = 0xFF;
  MemorySource overflowingCount(std::move(bytes));
  EXPECT_FALSE(pack.open(overflowingCount));
}

TEST(TriviaCore, RejectsOversizedRecordWithoutAffectingNextRecord) {
  Record oversized = questionRecord(1, false, false);
  oversized.fields[0].assign(trivia::kMaxRecordBytes + 1, 'x');
  MemorySource source(buildPack({oversized, questionRecord(2, false, false)}));
  trivia::Pack pack;
  ASSERT_TRUE(pack.open(source));

  trivia::Question question;
  EXPECT_FALSE(pack.read(0, question));
  ASSERT_TRUE(pack.read(1, question));
  EXPECT_EQ(question.difficulty(), 2);
}

TEST(TriviaCore, TracksStateFlagsAndRejectsMismatchedSizes) {
  MemorySource source(std::vector<uint8_t>(4, 0));
  trivia::PackState state;
  ASSERT_TRUE(state.open(source, 4));
  ASSERT_TRUE(state.setFlag(1, trivia::kSeen));
  ASSERT_TRUE(state.setFlag(1, trivia::kFlagged));
  EXPECT_EQ(state.seenCount(), 1);
  EXPECT_EQ(state.flaggedCount(), 1);
  const int writes = source.writes;
  ASSERT_TRUE(state.setFlag(1, trivia::kSeen));
  EXPECT_EQ(source.writes, writes);
  ASSERT_TRUE(state.clearFlag(1, trivia::kFlagged));
  EXPECT_EQ(state.flaggedCount(), 0);

  MemorySource shortSource(std::vector<uint8_t>(3, 0));
  trivia::PackState mismatched;
  EXPECT_FALSE(mismatched.open(shortSource, 4));

  MemorySource unreadable(std::vector<uint8_t>(4, 0));
  unreadable.failReadOffset = 0;
  EXPECT_FALSE(mismatched.open(unreadable, 4));
}

TEST(TriviaCore, ChooserHonorsModeDifficultyAndUsCentricFilter) {
  MemorySource packSource(buildPack({questionRecord(1, false, false), questionRecord(2, true, true),
                                     questionRecord(3, false, true)}));
  MemorySource stateSource(std::vector<uint8_t>(3, 0));
  trivia::Pack pack;
  trivia::PackState state;
  ASSERT_TRUE(pack.open(packSource));
  ASSERT_TRUE(state.open(stateSource, 3));
  trivia::Rng rng(11);
  trivia::Chooser chooser;
  chooser.begin(pack, state, rng);

  uint32_t index = 0;
  ASSERT_TRUE(chooser.next(index, true, 0, false));
  trivia::Question question;
  ASSERT_TRUE(pack.read(index, question));
  EXPECT_EQ(question.difficulty(), 3);
  EXPECT_FALSE(question.usCentric());

  ASSERT_TRUE(chooser.next(index, false, 2, true));
  ASSERT_TRUE(pack.read(index, question));
  EXPECT_EQ(question.difficulty(), 2);
  EXPECT_TRUE(question.usCentric());
}

TEST(TriviaCore, BuildsFourChoicesAndTracksCorrectOption) {
  MemorySource source(buildPack({questionRecord(2, false, true)}));
  trivia::Pack pack;
  ASSERT_TRUE(pack.open(source));
  trivia::Question question;
  ASSERT_TRUE(pack.read(0, question));

  trivia::Rng rng(7);
  trivia::Choices choices;
  ASSERT_TRUE(trivia::buildChoices(question, rng, choices));
  ASSERT_GE(choices.correct, 0);
  ASSERT_LT(choices.correct, trivia::kOptions);
  EXPECT_STREQ(choices.option[choices.correct], question.answer());
  for (int i = 0; i < trivia::kOptions; ++i) {
    ASSERT_NE(choices.option[i], nullptr);
    for (int j = i + 1; j < trivia::kOptions; ++j) EXPECT_STRNE(choices.option[i], choices.option[j]);
  }
}

TEST(TriviaCore, MapsBackAndPackRoomStates) {
  EXPECT_EQ(trivia::backFrom(trivia::View::Menu, true), trivia::Back::LeaveApp);
  EXPECT_EQ(trivia::backFrom(trivia::View::Solo, true), trivia::Back::EndRound);
  EXPECT_EQ(trivia::backFrom(trivia::View::Notice, false), trivia::Back::LeaveApp);
  EXPECT_EQ(trivia::backFrom(trivia::View::Notice, true), trivia::Back::ToMenu);
  EXPECT_EQ(trivia::backFrom(trivia::View::Reason, true), trivia::Back::ToNotice);

  EXPECT_EQ(trivia::roomFor(false, 0, 100), trivia::Room::Unknown);
  EXPECT_EQ(trivia::roomFor(true, 99, 100), trivia::Room::TooSmall);
  EXPECT_EQ(trivia::roomFor(true, 100, 100), trivia::Room::Ok);
}

}  // namespace

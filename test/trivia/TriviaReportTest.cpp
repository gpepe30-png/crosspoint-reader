#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "src/trivia/TriviaReport.h"

namespace {

class MemorySource final : public trivia::WritableByteSource {
 public:
  bool read(const uint32_t offset, void* dst, const uint32_t length) override {
    if (failRead || static_cast<uint64_t>(offset) + length > bytes.size()) return false;
    std::memcpy(dst, bytes.data() + offset, length);
    return true;
  }

  bool write(const uint32_t offset, const void* src, const uint32_t length) override {
    const uint64_t end = static_cast<uint64_t>(offset) + length;
    if (failWrite || end > UINT32_MAX) return false;
    if (end > bytes.size()) bytes.resize(static_cast<size_t>(end));
    std::memcpy(bytes.data() + offset, src, length);
    return true;
  }

  bool flush() override { return !failFlush; }
  uint32_t size() const override { return static_cast<uint32_t>(bytes.size()); }

  std::vector<uint8_t> bytes;
  bool failRead = false;
  bool failWrite = false;
  bool failFlush = false;
};

TEST(TriviaReport, QueuesAmendsWithdrawsAndResumesReports) {
  MemorySource source;
  trivia::ReportQueue queue;
  ASSERT_EQ(queue.open(source, "build-1", 8), trivia::QueueOpen::Started);
  ASSERT_TRUE(queue.add(3, trivia::Reason::None));
  ASSERT_TRUE(queue.add(3, trivia::Reason::Wrong));
  ASSERT_TRUE(queue.add(5, trivia::Reason::Broken));
  EXPECT_EQ(queue.count(), 2);
  EXPECT_EQ(queue.pending(), 2);
  ASSERT_TRUE(queue.withdraw(5));
  EXPECT_EQ(queue.pending(), 1);

  trivia::ReportQueue reopened;
  ASSERT_EQ(reopened.open(source, "build-1", 8), trivia::QueueOpen::Ready);
  EXPECT_EQ(reopened.pending(), 1);
  ASSERT_TRUE(reopened.markSent(2));
  EXPECT_EQ(reopened.pending(), 0);
  EXPECT_FALSE(reopened.add(3, trivia::Reason::Wrong));
  EXPECT_FALSE(reopened.withdraw(3));
  ASSERT_TRUE(reopened.add(7, trivia::Reason::None));
  EXPECT_EQ(reopened.pending(), 1);

  uint32_t index = 0;
  trivia::Reason reason = trivia::Reason::None;
  ASSERT_TRUE(reopened.entry(0, index, reason));
  EXPECT_EQ(index, 3);
  EXPECT_EQ(reason, trivia::Reason::Wrong);
}

TEST(TriviaReport, HoldsForeignPackReportsUntilDelivered) {
  MemorySource source;
  trivia::ReportQueue original;
  ASSERT_EQ(original.open(source, "old-build", 4), trivia::QueueOpen::Started);
  ASSERT_TRUE(original.add(1, trivia::Reason::Wrong));

  trivia::ReportQueue foreign;
  ASSERT_EQ(foreign.open(source, "new-build", 4), trivia::QueueOpen::Foreign);
  EXPECT_STREQ(foreign.packId(), "old-build");
  EXPECT_FALSE(foreign.add(2, trivia::Reason::Wrong));
  EXPECT_FALSE(foreign.setReason(1, trivia::Reason::Broken));
  EXPECT_FALSE(foreign.withdraw(1));
  ASSERT_TRUE(foreign.markSent(1));

  trivia::ReportQueue current;
  ASSERT_EQ(current.open(source, "new-build", 4), trivia::QueueOpen::Started);
  EXPECT_STREQ(current.packId(), "new-build");
  ASSERT_TRUE(current.add(2, trivia::Reason::Nonsense));
  EXPECT_EQ(current.pending(), 1);
}

TEST(TriviaReport, RejectsMalformedQueueAndStorageFailures) {
  MemorySource truncated;
  truncated.bytes.resize(1);
  trivia::ReportQueue bad;
  EXPECT_EQ(bad.open(truncated, "build", 1), trivia::QueueOpen::Unusable);
  EXPECT_EQ(bad.open(truncated, "build", 0), trivia::QueueOpen::Unusable);

  MemorySource source;
  trivia::ReportQueue queue;
  ASSERT_EQ(queue.open(source, "build", 1), trivia::QueueOpen::Started);
  source.failRead = true;
  EXPECT_FALSE(queue.add(0, trivia::Reason::Wrong));
  source.failRead = false;
  source.failWrite = true;
  EXPECT_FALSE(queue.add(0, trivia::Reason::Wrong));
  EXPECT_EQ(queue.count(), 0);
}

TEST(TriviaReport, RejectsInvalidStoredEntriesAndFullQueueAdditions) {
  MemorySource source;
  trivia::ReportQueue queue;
  ASSERT_EQ(queue.open(source, "build", 65), trivia::QueueOpen::Started);
  for (uint32_t index = 0; index < trivia::kMaxQueuedReports; ++index) {
    ASSERT_TRUE(queue.add(index, trivia::Reason::Wrong));
  }
  EXPECT_TRUE(queue.full());
  EXPECT_FALSE(queue.add(trivia::kMaxQueuedReports, trivia::Reason::Wrong));

  source.bytes[trivia::kReportHeaderBytes] = 65;
  source.bytes[trivia::kReportHeaderBytes + 1] = 0;
  source.bytes[trivia::kReportHeaderBytes + 2] = 0;
  source.bytes[trivia::kReportHeaderBytes + 3] = 0;
  trivia::ReportQueue reopened;
  EXPECT_EQ(reopened.open(source, "build", 65), trivia::QueueOpen::Unusable);
}

TEST(TriviaReport, ParsesAndValidatesPackMetadata) {
  char text[96] = {};
  const size_t length = trivia::formatMeta(text, sizeof(text), "build-42", 50, 900);
  ASSERT_GT(length, 0u);
  const trivia::PackMeta valid = trivia::parseMeta(text, length, 50, 900);
  EXPECT_TRUE(valid.valid);
  EXPECT_STREQ(valid.id, "build-42");
  EXPECT_FALSE(trivia::parseMeta(text, length, 49, 900).valid);
  EXPECT_EQ(trivia::formatMeta(text, 4, "build-42", 50, 900), 0u);
  EXPECT_EQ(trivia::formatMeta(text, sizeof(text), "../bad", 1, 1), 0u);
}

TEST(TriviaReport, FormatsOnlyPendingReportsAndSkipsWithdrawnEntries) {
  MemorySource source;
  trivia::ReportQueue queue;
  ASSERT_EQ(queue.open(source, "build-1", 10), trivia::QueueOpen::Started);
  ASSERT_TRUE(queue.add(2, trivia::Reason::None));
  ASSERT_TRUE(queue.add(4, trivia::Reason::Broken));
  ASSERT_TRUE(queue.add(6, trivia::Reason::Wrong));
  ASSERT_TRUE(queue.withdraw(6));
  ASSERT_TRUE(queue.markSent(1));

  char body[trivia::kMaxReportBatchBytes] = {};
  uint32_t reportCount = 0;
  const size_t length = trivia::formatReportBatch(queue, queue.count(), body, sizeof(body), reportCount);
  ASSERT_GT(length, 0u);
  EXPECT_EQ(reportCount, 1u);
  EXPECT_STREQ(body, R"({"pack":"build-1","count":10,"reports":[{"i":4,"r":"broken"}]})");
  EXPECT_EQ(trivia::formatReportBatch(queue, queue.count(), body, 8, reportCount), 0u);
}

TEST(TriviaReport, ParsesManifestAndComparesBuildFreshness) {
  constexpr char json[] = R"({"id":"a1b2c3d4e5f6","count":50,"bytes":900})";
  const trivia::PackManifest manifest = trivia::parseManifest(json, sizeof(json) - 1);
  ASSERT_TRUE(manifest.valid);
  EXPECT_STREQ(manifest.id, "a1b2c3d4e5f6");
  EXPECT_EQ(manifest.count, 50u);
  EXPECT_EQ(manifest.bytes, 900u);
  EXPECT_FALSE(trivia::parseManifest(R"({"id":"x","count":1x,"bytes":2})", 32).valid);
  constexpr char invalidId[] = R"({"id":"build-42","count":50,"bytes":900})";
  EXPECT_FALSE(trivia::parseManifest(invalidId, sizeof(invalidId) - 1).valid);

  trivia::PackMeta held;
  EXPECT_EQ(trivia::compare(held, manifest), trivia::Freshness::Unknown);
  held.valid = true;
  std::strcpy(held.id, "a1b2c3d4e5f6");
  held.count = 50;
  held.bytes = 900;
  EXPECT_EQ(trivia::compare(held, manifest), trivia::Freshness::Current);
  held.count = 49;
  EXPECT_EQ(trivia::compare(held, manifest), trivia::Freshness::Newer);
  std::strcpy(held.id, "previous");
  held.count = 50;
  EXPECT_EQ(trivia::compare(held, manifest), trivia::Freshness::Newer);
  EXPECT_EQ(trivia::compare(held, {}), trivia::Freshness::NoManifest);
}

}  // namespace

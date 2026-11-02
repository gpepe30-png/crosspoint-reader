#include "TriviaReport.h"

#include <cstdio>
#include <cstring>

namespace trivia {
namespace {

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeU16(uint8_t* p, const uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* p, const uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t entryOffset(const uint32_t slot) { return kReportHeaderBytes + slot * kReportEntryBytes; }

bool isNumberEnd(const char value) {
  return value == '\r' || value == '\n' || value == ' ' || value == '\t' || value == ',' || value == '}';
}

bool parseU32(const char* start, const char* end, uint32_t& result) {
  uint32_t value = 0;
  bool any = false;
  for (const char* p = start; p < end; ++p) {
    if (isNumberEnd(*p)) break;
    if (*p < '0' || *p > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(*p - '0');
    if (value > (UINT32_MAX - digit) / 10u) return false;
    value = value * 10u + digit;
    any = true;
  }
  if (!any) return false;
  result = value;
  return true;
}

bool copyId(char* dst, const char* src, const size_t length) {
  if (length == 0 || length > kPackIdBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = src[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
          c == '-'))
      return false;
    dst[i] = c;
  }
  dst[length] = '\0';
  return true;
}

bool isPackBuildId(const char* id) {
  constexpr size_t kBuildIdLength = 12;
  if (id == nullptr || std::strlen(id) != kBuildIdLength) return false;
  for (size_t i = 0; i < kBuildIdLength; ++i) {
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  }
  return true;
}

const char* valueAfter(const char* json, const size_t length, const char* key) {
  const size_t keyLength = std::strlen(key);
  if (length < keyLength + 3) return nullptr;
  for (size_t i = 0; i + keyLength + 2 <= length; ++i) {
    if (json[i] != '"' || std::strncmp(json + i + 1, key, keyLength) != 0 ||
        json[i + keyLength + 1] != '"')
      continue;
    size_t at = i + keyLength + 2;
    while (at < length && (json[at] == ' ' || json[at] == '\t')) ++at;
    if (at >= length || json[at++] != ':') continue;
    while (at < length && (json[at] == ' ' || json[at] == '\t')) ++at;
    return at < length ? json + at : nullptr;
  }
  return nullptr;
}

}  // namespace

const char* reasonName(const Reason reason) {
  switch (reason) {
    case Reason::None:
      return "none";
    case Reason::Wrong:
      return "wrong";
    case Reason::Nonsense:
      return "nonsense";
    case Reason::Giveaway:
      return "giveaway";
    case Reason::Ambiguous:
      return "ambiguous";
    case Reason::Outdated:
      return "outdated";
    case Reason::Broken:
      return "broken";
    case Reason::Regional:
      return "regional";
    case Reason::Us:
      return "us";
    case Reason::Hard:
      return "hard";
    case Reason::Easy:
      return "easy";
    case Reason::Count:
      return "";
  }
  return "";
}

bool ReportQueue::writeHeader() {
  uint8_t header[kReportHeaderBytes] = {};
  std::memcpy(header, kReportMagic, sizeof(kReportMagic));
  writeU16(header + 8, kReportVersion);
  writeU32(header + 12, packCount_);
  writeU32(header + 16, sent_);
  std::memcpy(header + 20, packId_, std::strlen(packId_));
  return source_->write(0, header, sizeof(header)) && source_->flush();
}

QueueOpen ReportQueue::open(WritableByteSource& source, const char* packId, const uint32_t packCount) {
  source_ = nullptr;
  count_ = sent_ = packCount_ = withdrawnPending_ = 0;
  acceptsReports_ = false;
  packId_[0] = '\0';
  if (packId == nullptr || packId[0] == '\0' || packCount == 0 ||
      !copyId(packId_, packId, std::strlen(packId)))
    return QueueOpen::Unusable;
  packCount_ = packCount;

  const uint32_t size = source.size();
  if (size == 0) {
    source_ = &source;
    if (writeHeader()) {
      acceptsReports_ = true;
      return QueueOpen::Started;
    }
    source_ = nullptr;
    return QueueOpen::Unusable;
  }
  if (size < kReportHeaderBytes) return QueueOpen::Unusable;

  uint8_t header[kReportHeaderBytes] = {};
  if (!source.read(0, header, sizeof(header)) || std::memcmp(header, kReportMagic, sizeof(kReportMagic)) != 0 ||
      readU16(header + 8) != kReportVersion)
    return QueueOpen::Unusable;

  const uint32_t body = size - kReportHeaderBytes;
  if (body % kReportEntryBytes != 0) return QueueOpen::Unusable;
  const uint32_t stored = body / kReportEntryBytes;
  const uint32_t storedPackCount = readU32(header + 12);
  const uint32_t storedSent = readU32(header + 16);
  if (storedPackCount == 0 || stored > kMaxQueuedReports || storedSent > stored) return QueueOpen::Unusable;

  char storedId[kPackIdBytes + 1] = {};
  size_t idLength = 0;
  while (idLength < kPackIdBytes && header[20 + idLength] != 0) ++idLength;
  if (!copyId(storedId, reinterpret_cast<const char*>(header + 20), idLength)) return QueueOpen::Unusable;

  uint32_t withdrawnPending = 0;
  for (uint32_t i = 0; i < stored; ++i) {
    uint8_t record[kReportEntryBytes] = {};
    if (!source.read(entryOffset(i), record, sizeof(record))) return QueueOpen::Unusable;
    const uint32_t index = readU32(record);
    if ((index != kWithdrawnIndex && index >= storedPackCount) ||
        record[4] >= static_cast<uint8_t>(Reason::Count))
      return QueueOpen::Unusable;
    if (i >= storedSent && index == kWithdrawnIndex) ++withdrawnPending;
  }

  source_ = &source;
  withdrawnPending_ = withdrawnPending;
  if (std::strcmp(storedId, packId_) == 0 && readU32(header + 12) == packCount) {
    count_ = stored;
    sent_ = storedSent;
    acceptsReports_ = true;
    return QueueOpen::Ready;
  }

  if (stored > storedSent) {
    std::memcpy(packId_, storedId, idLength + 1);
    packCount_ = storedPackCount;
    count_ = stored;
    sent_ = storedSent;
    return QueueOpen::Foreign;
  }

  for (uint32_t i = 0; i < stored; ++i) {
    uint8_t record[kReportEntryBytes] = {};
    writeU32(record, kWithdrawnIndex);
    if (!source.write(entryOffset(i), record, sizeof(record))) {
      source_ = nullptr;
      return QueueOpen::Unusable;
    }
  }
  count_ = sent_ = stored;
  if (!writeHeader()) {
    source_ = nullptr;
    return QueueOpen::Unusable;
  }
  acceptsReports_ = true;
  return QueueOpen::Started;
}

ReportQueue::FindResult ReportQueue::find(const uint32_t index, uint32_t& slotOut) const {
  for (uint32_t i = 0; i < count_; ++i) {
    uint8_t record[kReportEntryBytes] = {};
    if (!source_->read(entryOffset(i), record, sizeof(record))) return FindResult::ReadError;
    if (readU32(record) == index) {
      slotOut = i;
      return FindResult::Found;
    }
  }
  return FindResult::Missing;
}

bool ReportQueue::add(const uint32_t index, const Reason reason) {
  if (source_ == nullptr || !acceptsReports_ || packId_[0] == '\0' || index >= packCount_ ||
      reason >= Reason::Count)
    return false;
  uint32_t slot = 0;
  const FindResult found = find(index, slot);
  if (found == FindResult::ReadError) return false;
  if (found == FindResult::Found) {
    if (slot < sent_) return false;
    if (reason == Reason::None) return true;
    uint8_t record[kReportEntryBytes] = {};
    writeU32(record, index);
    record[4] = static_cast<uint8_t>(reason);
    return source_->write(entryOffset(slot), record, sizeof(record)) && source_->flush();
  }
  if (full()) return false;

  uint8_t record[kReportEntryBytes] = {};
  writeU32(record, index);
  record[4] = static_cast<uint8_t>(reason);
  if (!source_->write(entryOffset(count_), record, sizeof(record))) return false;
  ++count_;
  if (writeHeader()) return true;
  --count_;
  return false;
}

bool ReportQueue::setReason(const uint32_t index, const Reason reason) {
  if (source_ == nullptr || !acceptsReports_ || reason >= Reason::Count) return false;
  uint32_t slot = 0;
  const FindResult found = find(index, slot);
  if (found != FindResult::Found || slot < sent_) return false;
  uint8_t record[kReportEntryBytes] = {};
  writeU32(record, index);
  record[4] = static_cast<uint8_t>(reason);
  return source_->write(entryOffset(slot), record, sizeof(record)) && source_->flush();
}

bool ReportQueue::entry(const uint32_t slot, uint32_t& indexOut, Reason& reasonOut) const {
  if (source_ == nullptr || slot >= count_) return false;
  uint8_t record[kReportEntryBytes] = {};
  if (!source_->read(entryOffset(slot), record, sizeof(record)) || record[4] >= static_cast<uint8_t>(Reason::Count))
    return false;
  indexOut = readU32(record);
  reasonOut = static_cast<Reason>(record[4]);
  return true;
}

bool ReportQueue::withdraw(const uint32_t index) {
  if (source_ == nullptr || !acceptsReports_ || index == kWithdrawnIndex) return false;
  uint32_t slot = 0;
  const FindResult found = find(index, slot);
  if (found != FindResult::Found || slot < sent_) return false;
  uint8_t record[kReportEntryBytes] = {};
  writeU32(record, kWithdrawnIndex);
  if (!source_->write(entryOffset(slot), record, sizeof(record)) || !source_->flush()) return false;
  ++withdrawnPending_;
  return true;
}

bool ReportQueue::markSent(const uint32_t count) {
  if (source_ == nullptr || count > count_ || count < sent_) return false;
  uint32_t withdrawnPending = 0;
  for (uint32_t i = count; i < count_; ++i) {
    uint8_t record[kReportEntryBytes] = {};
    if (!source_->read(entryOffset(i), record, sizeof(record))) return false;
    if (readU32(record) == kWithdrawnIndex) ++withdrawnPending;
  }
  const uint32_t previousSent = sent_;
  const uint32_t previousWithdrawn = withdrawnPending_;
  sent_ = count;
  withdrawnPending_ = withdrawnPending;
  if (writeHeader()) return true;
  sent_ = previousSent;
  withdrawnPending_ = previousWithdrawn;
  return false;
}

PackMeta parseMeta(const char* text, const size_t length, const uint32_t actualCount, const uint32_t actualBytes) {
  PackMeta meta;
  if (text == nullptr || length == 0) return meta;
  bool haveId = false;
  bool haveCount = false;
  bool haveBytes = false;
  const char* p = text;
  const char* end = text + length;
  while (p < end) {
    const char* lineEnd = p;
    while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
    const char* tab = p;
    while (tab < lineEnd && *tab != '\t') ++tab;
    if (tab < lineEnd) {
      const size_t keyLength = static_cast<size_t>(tab - p);
      const char* value = tab + 1;
      if (keyLength == 2 && std::strncmp(p, "id", 2) == 0) {
        size_t valueLength = 0;
        while (value + valueLength < lineEnd && value[valueLength] != '\r') ++valueLength;
        haveId = copyId(meta.id, value, valueLength);
      } else if (keyLength == 5 && std::strncmp(p, "count", 5) == 0) {
        haveCount = parseU32(value, lineEnd, meta.count);
      } else if (keyLength == 5 && std::strncmp(p, "bytes", 5) == 0) {
        haveBytes = parseU32(value, lineEnd, meta.bytes);
      }
    }
    p = lineEnd < end ? lineEnd + 1 : end;
  }
  meta.valid = haveId && haveCount && haveBytes && meta.count == actualCount && meta.bytes == actualBytes;
  return meta;
}

size_t formatMeta(char* out, const size_t capacity, const char* id, const uint32_t count, const uint32_t bytes) {
  if (out == nullptr || capacity == 0 || id == nullptr) return 0;
  char checkedId[kPackIdBytes + 1] = {};
  if (!copyId(checkedId, id, std::strlen(id))) return 0;
  const int written = std::snprintf(out, capacity, "id\t%s\ncount\t%u\nbytes\t%u\n", checkedId,
                                   static_cast<unsigned>(count), static_cast<unsigned>(bytes));
  return written <= 0 || static_cast<size_t>(written) >= capacity ? 0 : static_cast<size_t>(written);
}

size_t formatReportBatch(const ReportQueue& queue, const uint32_t through, char* out, const size_t capacity,
                         uint32_t& reportCount) {
  reportCount = 0;
  if (!queue.isOpen() || out == nullptr || capacity == 0 || through < queue.sent() || through > queue.count())
    return 0;

  size_t used = 0;
  const int header = std::snprintf(out, capacity, "{\"pack\":\"%s\",\"count\":%u,\"reports\":[", queue.packId(),
                                   static_cast<unsigned>(queue.packCount()));
  if (header <= 0 || static_cast<size_t>(header) >= capacity) return 0;
  used = static_cast<size_t>(header);

  for (uint32_t slot = queue.sent(); slot < through; ++slot) {
    uint32_t index = 0;
    Reason reason = Reason::None;
    if (!queue.entry(slot, index, reason)) return 0;
    if (index == kWithdrawnIndex) continue;

    const int written =
        reason == Reason::None
            ? std::snprintf(out + used, capacity - used, "%s{\"i\":%u}", reportCount == 0 ? "" : ",",
                            static_cast<unsigned>(index))
            : std::snprintf(out + used, capacity - used, "%s{\"i\":%u,\"r\":\"%s\"}",
                            reportCount == 0 ? "" : ",", static_cast<unsigned>(index), reasonName(reason));
    if (written <= 0 || static_cast<size_t>(written) >= capacity - used) return 0;
    used += static_cast<size_t>(written);
    ++reportCount;
  }

  if (reportCount == 0) return 0;
  const int footer = std::snprintf(out + used, capacity - used, "]}");
  if (footer != 2 || static_cast<size_t>(footer) >= capacity - used) return 0;
  return used + static_cast<size_t>(footer);
}

PackManifest parseManifest(const char* json, const size_t length) {
  PackManifest manifest;
  if (json == nullptr || length == 0) return manifest;
  const char* idAt = valueAfter(json, length, "id");
  const char* countAt = valueAfter(json, length, "count");
  const char* bytesAt = valueAfter(json, length, "bytes");
  const char* end = json + length;
  if (idAt == nullptr || countAt == nullptr || bytesAt == nullptr || *idAt != '"') return manifest;
  ++idAt;
  const char* idEnd = idAt;
  while (idEnd < end && *idEnd != '"') ++idEnd;
  if (idEnd == end || !copyId(manifest.id, idAt, static_cast<size_t>(idEnd - idAt)) ||
      !isPackBuildId(manifest.id) ||
      !parseU32(countAt, end, manifest.count) || !parseU32(bytesAt, end, manifest.bytes))
    return PackManifest{};
  manifest.valid = manifest.count > 0 && manifest.bytes > 0;
  return manifest;
}

Freshness compare(const PackMeta& held, const PackManifest& published) {
  if (!published.valid) return Freshness::NoManifest;
  if (!held.valid) return Freshness::Unknown;
  return std::strcmp(held.id, published.id) == 0 && held.count == published.count && held.bytes == published.bytes
             ? Freshness::Current
             : Freshness::Newer;
}

}  // namespace trivia

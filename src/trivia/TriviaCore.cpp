#include "TriviaCore.h"

#include <cstring>

namespace trivia {
namespace {

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool sameText(const char* a, const char* b) {
  if (a == nullptr || b == nullptr) return false;
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

}  // namespace

bool Pack::open(ByteSource& source) {
  source_ = nullptr;
  count_ = 0;
  blobBytes_ = 0;

  uint8_t header[16] = {};
  if (!source.read(0, header, sizeof(header))) return false;
  for (int i = 0; i < 8; ++i) {
    if (header[i] != kMagic[i]) return false;
  }
  if (readU16(header + 8) != kFormatVersion) return false;

  const uint32_t count = readU32(header + 12);
  if (count == 0) return false;

  const uint64_t indexBytes = (static_cast<uint64_t>(count) + 1) * 4;
  if (sizeof(header) + indexBytes > source.size()) return false;

  uint8_t sentinel[4] = {};
  if (!source.read(static_cast<uint32_t>(sizeof(header) + static_cast<uint64_t>(count) * 4), sentinel, sizeof(sentinel)))
    return false;
  const uint32_t blobBytes = readU32(sentinel);
  if (static_cast<uint64_t>(sizeof(header)) + indexBytes + blobBytes > source.size()) return false;

  source_ = &source;
  count_ = count;
  indexAt_ = sizeof(header);
  base_ = static_cast<uint32_t>(sizeof(header) + indexBytes);
  blobBytes_ = blobBytes;
  return true;
}

bool Pack::read(const uint32_t index, Question& out) const {
  if (source_ == nullptr || index >= count_) return false;

  uint8_t pair[8] = {};
  if (!source_->read(indexAt_ + index * 4u, pair, sizeof(pair))) return false;
  const uint32_t start = readU32(pair);
  const uint32_t end = readU32(pair + 4);
  if (end <= start || end > blobBytes_) return false;

  const uint32_t length = end - start;
  if (length > kMaxRecordBytes) return false;
  if (!source_->read(base_ + start, out.bytes_, length)) return false;
  if (length < 5) return false;

  const auto* raw = reinterpret_cast<const uint8_t*>(out.bytes_);
  out.difficulty_ = static_cast<uint8_t>(raw[0] & kDifficultyMask);
  out.usCentric_ = (raw[0] & kUsCentric) != 0;
  out.year_ = readU16(raw + 1);
  out.altCount_ = raw[3];
  out.wrongCount_ = raw[4];
  if (out.altCount_ > kMaxAlternates || out.wrongCount_ > kMaxDistractors) return false;

  const int fields = 2 + out.altCount_ + out.wrongCount_;
  uint32_t in = 5;
  uint32_t outAt = 0;
  for (int i = 0; i < fields; ++i) {
    if (in + 2 > length) return false;
    const uint16_t fieldLength = readU16(raw + in);
    in += 2;
    if (in + fieldLength > length || outAt + fieldLength + 1 > kMaxRecordBytes) return false;
    out.offset_[i] = static_cast<uint16_t>(outAt);
    std::memmove(out.bytes_ + outAt, out.bytes_ + in, fieldLength);
    out.bytes_[outAt + fieldLength] = '\0';
    outAt += fieldLength + 1;
    in += fieldLength;
  }
  out.fieldCount_ = fields;
  return true;
}

bool PackState::scanCounts() {
  seenCount_ = 0;
  flaggedCount_ = 0;
  if (source_ == nullptr) return false;
  uint8_t window[128];
  for (uint32_t at = 0; at < count_;) {
    const uint32_t want = (count_ - at) < sizeof(window) ? (count_ - at) : sizeof(window);
    if (!source_->read(at, window, want)) return false;
    for (uint32_t i = 0; i < want; ++i) {
      if ((window[i] & kSeen) != 0) ++seenCount_;
      if ((window[i] & kFlagged) != 0) ++flaggedCount_;
    }
    at += want;
  }
  return true;
}

bool PackState::open(WritableByteSource& source, const uint32_t count) {
  source_ = nullptr;
  count_ = 0;
  seenCount_ = 0;
  flaggedCount_ = 0;
  if (count == 0 || source.size() != count) return false;
  source_ = &source;
  count_ = count;
  if (!scanCounts()) {
    source_ = nullptr;
    count_ = 0;
    seenCount_ = 0;
    flaggedCount_ = 0;
    return false;
  }
  return true;
}

bool PackState::flags(const uint32_t index, uint8_t& result) const {
  return source_ != nullptr && index < count_ && source_->read(index, &result, 1);
}

bool PackState::setFlag(const uint32_t index, const uint8_t bit) {
  if (source_ == nullptr || index >= count_) return false;
  uint8_t byte = 0;
  if (!source_->read(index, &byte, 1)) return false;
  const uint8_t updated = static_cast<uint8_t>(byte | bit);
  if (updated == byte) return true;
  if (!source_->write(index, &updated, 1) || !source_->flush()) return false;
  if ((bit & kSeen) != 0 && (byte & kSeen) == 0) ++seenCount_;
  if ((bit & kFlagged) != 0 && (byte & kFlagged) == 0) ++flaggedCount_;
  return true;
}

bool PackState::clearFlag(const uint32_t index, const uint8_t bit) {
  if (source_ == nullptr || index >= count_) return false;
  uint8_t byte = 0;
  if (!source_->read(index, &byte, 1)) return false;
  const uint8_t updated = static_cast<uint8_t>(byte & ~bit);
  if (updated == byte) return true;
  if (!source_->write(index, &updated, 1) || !source_->flush()) return false;
  if ((bit & kSeen) != 0 && (byte & kSeen) != 0 && seenCount_ > 0) --seenCount_;
  if ((bit & kFlagged) != 0 && (byte & kFlagged) != 0 && flaggedCount_ > 0) --flaggedCount_;
  return true;
}

uint32_t Rng::next() {
  state_ ^= state_ << 13;
  state_ ^= state_ >> 17;
  state_ ^= state_ << 5;
  return state_;
}

uint32_t Rng::below(const uint32_t bound) { return bound == 0 ? 0 : next() % bound; }

void Chooser::begin(const Pack& pack, PackState& state, Rng& rng) {
  pack_ = &pack;
  state_ = &state;
  rng_ = &rng;
}

bool Chooser::next(uint32_t& indexOut, const bool requireChoice, const int difficulty, const bool allowUsCentric) {
  if (pack_ == nullptr || state_ == nullptr || rng_ == nullptr || pack_->count() == 0) return false;
  const uint32_t count = pack_->count();
  const uint32_t start = rng_->below(count);

  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t step = 0; step < count; ++step) {
      const uint32_t i = (start + step) % count;
      uint8_t flags = 0;
      if (!state_->flags(i, flags)) return false;
      if ((flags & kFlagged) != 0 || (pass == 0 && (flags & kSeen) != 0)) continue;
      if (!pack_->read(i, candidate_)) continue;
      if (!allowUsCentric && candidate_.usCentric()) continue;
      if (requireChoice && !candidate_.playableAsChoice()) continue;
      if (difficulty != 0 && candidate_.difficulty() != difficulty) continue;
      indexOut = i;
      return true;
    }
  }
  return false;
}

bool buildChoices(const Question& question, Rng& rng, Choices& out) {
  if (!question.playableAsChoice()) return false;

  int pool[kMaxDistractors];
  const int count = question.distractorCount();
  for (int i = 0; i < count; ++i) pool[i] = i;
  for (int i = 0; i < kOptions - 1; ++i) {
    const int j = i + static_cast<int>(rng.below(static_cast<uint32_t>(count - i)));
    const int temp = pool[i];
    pool[i] = pool[j];
    pool[j] = temp;
  }

  out.option[0] = question.answer();
  for (int i = 0; i < kOptions - 1; ++i) out.option[i + 1] = question.distractor(pool[i]);

  for (int i = kOptions - 1; i > 0; --i) {
    const int j = static_cast<int>(rng.below(static_cast<uint32_t>(i + 1)));
    const char* temp = out.option[i];
    out.option[i] = out.option[j];
    out.option[j] = temp;
  }
  for (int i = 0; i < kOptions; ++i) {
    if (sameText(out.option[i], question.answer())) out.correct = i;
  }
  return true;
}

bool answerMatches(const Question& question, const char* given) {
  if (sameText(question.answer(), given)) return true;
  for (int i = 0; i < question.alternateCount(); ++i) {
    if (sameText(question.alternate(i), given)) return true;
  }
  return false;
}

}  // namespace trivia

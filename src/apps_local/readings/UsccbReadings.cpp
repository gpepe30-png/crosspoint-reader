#include "UsccbReadings.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <ResumableFetch.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "network/HttpDownloader.h"
#include "network/WifiPowerSaveGuard.h"
#include "apps_local/study/StudySyncRoots.h"

namespace usccb {

namespace {

constexpr char kCacheDirectory[] = "/readings/usccb";
constexpr size_t kMaxPageBytes = 512 * 1024;
constexpr size_t kMaxReadingBytes = 64 * 1024;
constexpr size_t kPageReadBufferBytes = 192;
constexpr size_t kOutputBufferBytes = 256;
constexpr size_t kTagBufferBytes = 512;
constexpr int64_t kClockFloor = 1700000000;
constexpr uint32_t kHttpTimeoutMs = 60000;

enum class Capture : uint8_t { None, PageTitle, Heading, Address, Body };
enum class RawElement : uint8_t { None, Script, Style };

class ReadingExtractor {
 public:
  explicit ReadingExtractor(HalFile& file) : file_(file) {}

  bool consume(const uint8_t* bytes, const size_t size) {
    for (size_t i = 0; i < size && !failed_; ++i) consumeByte(static_cast<char>(bytes[i]));
    return !failed_;
  }

  bool finish() {
    if (entityLength_ > 0) flushEntity();
    return !failed_ && bodyCount_ > 0 && flush();
  }

 private:
  bool append(const char* text, const size_t size) {
    if (written_ + buffered_ + size > kMaxReadingBytes) {
      failed_ = true;
      return false;
    }
    for (size_t i = 0; i < size; ++i) {
      const char c = text[i];
      if (c == '\r') continue;
      if (c == '\n' && (buffered_ == 0 ? lastWritten_ : output_[buffered_ - 1]) == '\n') continue;
      if (c == ' ' && (buffered_ == 0 ? lastWritten_ : output_[buffered_ - 1]) == ' ') continue;
      output_[buffered_++] = c;
      if (buffered_ == sizeof(output_) && !flush()) return false;
    }
    return true;
  }

  bool flush() {
    if (buffered_ == 0) return true;
    if (file_.write(reinterpret_cast<const uint8_t*>(output_), buffered_) != buffered_) {
      failed_ = true;
      return false;
    }
    lastWritten_ = output_[buffered_ - 1];
    written_ += buffered_;
    buffered_ = 0;
    return true;
  }

  void newline(const bool paragraph = false) {
    if (buffered_ == 0 && lastWritten_ == '\n') return;
    if (buffered_ > 0 && output_[buffered_ - 1] == '\n') return;
    append("\n", 1);
    if (paragraph) append("\n", 1);
  }

  void appendCapturedText(const char* text, const size_t size) {
    if (capture_ == Capture::Address) {
      for (size_t i = 0; i < size; ++i) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) {
          addressPendingSpace_ = addressHasText_;
          continue;
        }
        if (addressPendingSpace_) append(" ", 1);
        append(text + i, 1);
        addressHasText_ = true;
        addressPendingSpace_ = false;
      }
      return;
    }
    if (capture_ != Capture::PageTitle) {
      append(text, size);
      return;
    }
    if (pageTitleOverflow_) return;
    if (size >= sizeof(pageTitle_) - pageTitleLength_) {
      pageTitleOverflow_ = true;
      return;
    }
    std::memcpy(pageTitle_ + pageTitleLength_, text, size);
    pageTitleLength_ += size;
  }

  void finishPageTitle() {
    capture_ = Capture::None;
    if (pageTitleOverflow_) return;

    size_t normalizedLength = 0;
    bool pendingSpace = false;
    for (size_t i = 0; i < pageTitleLength_; ++i) {
      const unsigned char c = static_cast<unsigned char>(pageTitle_[i]);
      if (std::isspace(c)) {
        pendingSpace = normalizedLength > 0;
        continue;
      }
      if (pendingSpace) pageTitle_[normalizedLength++] = ' ';
      pageTitle_[normalizedLength++] = static_cast<char>(c);
      pendingSpace = false;
    }
    pageTitle_[normalizedLength] = '\0';

    if (equalsIgnoreCase(pageTitle_, "menu: top buttons") || equalsIgnoreCase(pageTitle_, "menu: top") ||
        equalsIgnoreCase(pageTitle_, "main navigation")) {
      return;
    }

    newline(true);
    append("## **", 5);
    append(pageTitle_, normalizedLength);
    append("**", 2);
    newline(true);
  }

  bool equalsIgnoreCase(const char* text, const char* expected) const {
    while (*text != '\0' && *expected != '\0' &&
           std::tolower(static_cast<unsigned char>(*text)) == std::tolower(static_cast<unsigned char>(*expected))) {
      ++text;
      ++expected;
    }
    return *text == '\0' && *expected == '\0';
  }

  void consumeByte(const char c) {
    if (comment_) {
      if (c == '>') {
        if (commentDashCount_ >= 2) comment_ = false;
        commentDashCount_ = 0;
      } else {
        commentDashCount_ = c == '-' ? commentDashCount_ + 1 : 0;
      }
      return;
    }
    if (rawElement_ != RawElement::None) {
      consumeRawByte(c);
      return;
    }
    if (inTag_) {
      if (tagLength_ < sizeof(tag_) - 1) {
        tag_[tagLength_++] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      } else {
        tagOverflow_ = true;
      }
      if (tagLength_ == 4 && std::memcmp(tag_, "<!--", 4) == 0) {
        inTag_ = false;
        comment_ = true;
        commentDashCount_ = 0;
        return;
      }
      if (tagOverflow_) {
        if (c == '>') resetTag();
        return;
      }
      if (quote_ != '\0') {
        if (c == quote_) quote_ = '\0';
        return;
      }
      if (c == '\'' || c == '"') {
        quote_ = c;
        return;
      }
      if (c == '>') {
        tag_[tagLength_] = '\0';
        handleTag();
        resetTag();
      }
      return;
    }
    if (c == '<') {
      inTag_ = true;
      tagLength_ = 0;
      tagOverflow_ = false;
      quote_ = '\0';
      tag_[tagLength_++] = '<';
      return;
    }
    if (capture_ != Capture::None) consumeTextByte(c);
  }

  void consumeRawByte(const char c) {
    static constexpr char kScriptEnd[] = "</script>";
    static constexpr char kStyleEnd[] = "</style>";
    const char* end = rawElement_ == RawElement::Script ? kScriptEnd : kStyleEnd;
    if (c == end[rawMatchLength_]) {
      ++rawMatchLength_;
      if (end[rawMatchLength_] == '\0') {
        rawElement_ = RawElement::None;
        rawMatchLength_ = 0;
      }
    } else {
      rawMatchLength_ = c == '<' ? 1 : 0;
    }
  }

  void resetTag() {
    inTag_ = false;
    tagLength_ = 0;
    tagOverflow_ = false;
    quote_ = '\0';
  }

  bool hasClass(const char* tag, const char* wanted) const {
    const char* attr = std::strstr(tag, "class=");
    if (attr == nullptr) return false;
    attr += 6;
    char delimiter = '\0';
    if (*attr == '\'' || *attr == '"') delimiter = *attr++;
    const char* end = delimiter != '\0' ? std::strchr(attr, delimiter) : std::strpbrk(attr, " />");
    const size_t length = end != nullptr ? static_cast<size_t>(end - attr) : std::strlen(attr);
    const char* match = std::strstr(attr, wanted);
    return match != nullptr && static_cast<size_t>(match - attr) < length &&
           static_cast<size_t>(match - attr) + std::strlen(wanted) <= length;
  }

  void handleTag() {
    const char* cursor = tag_ + 1;
    bool closing = false;
    if (*cursor == '/') {
      closing = true;
      ++cursor;
    }
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    char name[16] = {};
    size_t nameLength = 0;
    while (*cursor != '\0' && nameLength + 1 < sizeof(name) &&
           std::isalnum(static_cast<unsigned char>(*cursor))) {
      name[nameLength++] = *cursor++;
    }
    if (nameLength == 0) return;

    if (std::strcmp(name, "h2") == 0 && verseDepth_ < 0 && bodyCount_ == 0) {
      if (closing && capture_ == Capture::PageTitle) {
        finishPageTitle();
      } else if (!closing && capture_ == Capture::None) {
        capture_ = Capture::PageTitle;
        pageTitleLength_ = 0;
        pageTitleOverflow_ = false;
      }
      return;
    }

    if (rawElement_ != RawElement::None) return;
    if (!closing && (std::strcmp(name, "script") == 0 || std::strcmp(name, "style") == 0)) {
      rawElement_ = std::strcmp(name, "script") == 0 ? RawElement::Script : RawElement::Style;
      rawMatchLength_ = 0;
      return;
    }

    if (std::strcmp(name, "div") == 0) {
      if (closing) {
        if (captureDepth_ == divDepth_) {
          if (capture_ == Capture::Address) {
            append("*", 1);
            addressHasText_ = false;
            addressPendingSpace_ = false;
          }
          capture_ = Capture::None;
          captureDepth_ = -1;
          newline(captureWasBody_);
          if (captureWasBody_) ++bodyCount_;
          captureWasBody_ = false;
        }
        if (verseDepth_ == divDepth_) {
          verseDepth_ = -1;
          newline(true);
        }
        if (divDepth_ > 0) --divDepth_;
        return;
      }

      ++divDepth_;
      if (verseDepth_ < 0 && hasClass(tag_, "b-verse")) verseDepth_ = divDepth_;
      if (verseDepth_ >= 0 && hasClass(tag_, "address")) {
        capture_ = Capture::Address;
        captureDepth_ = divDepth_;
        addressHasText_ = false;
        addressPendingSpace_ = false;
        newline();
        append("*", 1);
      } else if (verseDepth_ >= 0 && hasClass(tag_, "content-body")) {
        capture_ = Capture::Body;
        captureDepth_ = divDepth_;
        captureWasBody_ = true;
        newline();
      }
      return;
    }

    if (verseDepth_ >= 0 && std::strcmp(name, "h3") == 0 && hasClass(tag_, "name")) {
      if (closing) {
        append("**", 2);
        capture_ = Capture::None;
        newline();
      } else {
        capture_ = Capture::Heading;
        newline(true);
        if (readingHeadingSeen_) append("\f", 1);
        append("### **", 6);
        readingHeadingSeen_ = true;
      }
      return;
    }

    if (capture_ == Capture::Body) {
      if ((std::strcmp(name, "br") == 0 || std::strcmp(name, "p") == 0) && !closing) newline();
      if (std::strcmp(name, "p") == 0 && closing) newline();
    }
  }

  void consumeTextByte(const char c) {
    if (entityLength_ > 0) {
      if (c == ';') {
        decodeEntity();
        return;
      }
      if (entityLength_ + 1 < sizeof(entity_)) {
        entity_[entityLength_++] = c;
        return;
      }
      flushEntity();
    }
    if (c == '&') {
      entity_[0] = '&';
      entityLength_ = 1;
    } else {
      appendCapturedText(&c, 1);
    }
  }

  void flushEntity() {
    appendCapturedText(entity_, entityLength_);
    entityLength_ = 0;
  }

  void appendCodepoint(const uint32_t codepoint) {
    char encoded[4];
    size_t count = 0;
    if (codepoint <= 0x7f) {
      encoded[count++] = static_cast<char>(codepoint);
    } else if (codepoint <= 0x7ff) {
      encoded[count++] = static_cast<char>(0xc0 | (codepoint >> 6));
      encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
      encoded[count++] = static_cast<char>(0xe0 | (codepoint >> 12));
      encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
      encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0x10ffff) {
      encoded[count++] = static_cast<char>(0xf0 | (codepoint >> 18));
      encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
      encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
      encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
    }
    if (count > 0) appendCapturedText(encoded, count);
  }

  void decodeEntity() {
    entity_[entityLength_] = '\0';
    const char* name = entity_ + 1;
    if (std::strcmp(name, "nbsp") == 0) {
      append(" ", 1);
    } else if (std::strcmp(name, "amp") == 0) {
      append("&", 1);
    } else if (std::strcmp(name, "quot") == 0) {
      append("\"", 1);
    } else if (std::strcmp(name, "apos") == 0) {
      append("'", 1);
    } else if (std::strcmp(name, "rsquo") == 0) {
      appendCodepoint(0x2019);
    } else if (std::strcmp(name, "lsquo") == 0) {
      appendCodepoint(0x2018);
    } else if (std::strcmp(name, "rdquo") == 0) {
      appendCodepoint(0x201d);
    } else if (std::strcmp(name, "ldquo") == 0) {
      appendCodepoint(0x201c);
    } else if (std::strcmp(name, "mdash") == 0) {
      appendCodepoint(0x2014);
    } else if (std::strcmp(name, "ndash") == 0) {
      appendCodepoint(0x2013);
    } else if (std::strcmp(name, "copy") == 0) {
      appendCodepoint(0x00a9);
    } else if (name[0] == '#') {
      char* end = nullptr;
      const int base = name[1] == 'x' ? 16 : 10;
      const char* digits = name + (base == 16 ? 2 : 1);
      const unsigned long codepoint = std::strtoul(digits, &end, base);
      if (end != digits && *end == '\0' && codepoint <= 0x10ffff) {
        appendCodepoint(static_cast<uint32_t>(codepoint));
      } else {
        flushEntity();
        return;
      }
    } else {
      flushEntity();
      return;
    }
    entityLength_ = 0;
  }

  HalFile& file_;
  char output_[kOutputBufferBytes] = {};
  size_t buffered_ = 0;
  size_t written_ = 0;
  char lastWritten_ = '\0';
  char tag_[kTagBufferBytes] = {};
  size_t tagLength_ = 0;
  char entity_[20] = {};
  char pageTitle_[128] = {};
  size_t entityLength_ = 0;
  size_t pageTitleLength_ = 0;
  size_t rawMatchLength_ = 0;
  int divDepth_ = 0;
  int verseDepth_ = -1;
  int captureDepth_ = -1;
  int bodyCount_ = 0;
  Capture capture_ = Capture::None;
  RawElement rawElement_ = RawElement::None;
  char quote_ = '\0';
  bool inTag_ = false;
  bool tagOverflow_ = false;
  bool comment_ = false;
  bool captureWasBody_ = false;
  bool pageTitleOverflow_ = false;
  bool readingHeadingSeen_ = false;
  bool addressHasText_ = false;
  bool addressPendingSpace_ = false;
  int commentDashCount_ = 0;
  bool failed_ = false;
};

bool parsePageFile(const char* rawPath, const char* textTempPath) {
  HalFile input = Storage.open(rawPath, O_RDONLY);
  if (!input.isOpen()) return false;
  HalFile output = Storage.open(textTempPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!output.isOpen()) return false;

  ReadingExtractor extractor(output);
  uint8_t buffer[kPageReadBufferBytes];
  size_t remaining = input.size();
  bool readComplete = true;
  while (remaining > 0) {
    const size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const int count = input.read(buffer, requested);
    if (count <= 0 || !extractor.consume(buffer, static_cast<size_t>(count))) {
      readComplete = false;
      break;
    }
    remaining -= static_cast<size_t>(count);
  }

  const bool parsed = readComplete && remaining == 0 && extractor.finish();
  input.close();
  output.flush();
  output.close();
  if (!parsed) Storage.remove(textTempPath);
  return parsed;
}

}  // namespace

bool makeWindow(const std::time_t now, Day out[kWindowSize]) {
  if (now < kClockFloor) return false;
  std::tm today{};
  if (localtime_r(&now, &today) == nullptr) return false;

  for (size_t i = 0; i < kWindowSize; ++i) {
    std::tm date = today;
    date.tm_mday += static_cast<int>(i) - 3;
    date.tm_hour = 12;
    date.tm_min = 0;
    date.tm_sec = 0;
    const std::time_t dayTime = std::mktime(&date);
    if (dayTime == static_cast<std::time_t>(-1) || localtime_r(&dayTime, &date) == nullptr) return false;
    out[i] = Day{date.tm_year + 1900, date.tm_mon + 1, date.tm_mday};
  }
  return true;
}

std::string cachePath(const Day& day) {
  char path[64];
  std::snprintf(path, sizeof(path), "%s/daily_readings_%02d%02d%02d.md", kCacheDirectory, day.month, day.day,
                day.year % 100);
  return path;
}

bool isCached(const Day& day) {
  const std::string path = cachePath(day);
  if (Storage.exists(path.c_str())) return true;

  char legacyPath[48];
  std::snprintf(legacyPath, sizeof(legacyPath), "%s/%04d%02d%02d.txt", kCacheDirectory, day.year, day.month,
                day.day);
  if (!Storage.exists(legacyPath)) return false;
  return Storage.rename(legacyPath, path.c_str()) || Storage.exists(path.c_str());
}

namespace {

bool cacheFilenameDateKey(const char* filename, int& dateKey) {
  constexpr char kFilenamePrefix[] = "daily_readings_";
  const bool isNewName = std::strncmp(filename, kFilenamePrefix, sizeof(kFilenamePrefix) - 1) == 0;
  const char* digits = isNewName ? filename + sizeof(kFilenamePrefix) - 1 : filename;
  const size_t digitCount = isNewName ? 6 : 8;
  int month = 0;
  int day = 0;
  int year = 0;
  for (size_t digit = 0; digit < digitCount; ++digit) {
    if (digits[digit] < '0' || digits[digit] > '9') return false;
    const int value = digits[digit] - '0';
    if (isNewName) {
      if (digit < 2) month = month * 10 + value;
      else if (digit < 4) day = day * 10 + value;
      else year = year * 10 + value;
    } else {
      if (digit < 4) year = year * 10 + value;
      else if (digit < 6) month = month * 10 + value;
      else if (digit >= 6) day = day * 10 + value;
    }
  }

  const char* suffix = digits + digitCount;
  const bool recognizedSuffix =
      isNewName ? std::strcmp(suffix, ".md") == 0 || std::strcmp(suffix, ".md.part") == 0 ||
                      std::strcmp(suffix, ".md.html.part") == 0
                : std::strcmp(suffix, ".txt") == 0 || std::strcmp(suffix, ".txt.part") == 0 ||
                      std::strcmp(suffix, ".txt.html.part") == 0;
  if (!recognizedSuffix || month < 1 || month > 12 || day < 1 || day > 31) return false;
  dateKey = (isNewName ? 2000 + year : year) * 10000 + month * 100 + day;
  return true;
}

}  // namespace

bool pruneExpiredCache(const Day& today) {
  if (!Storage.exists(kCacheDirectory)) return true;

  std::tm cutoffDate{};
  cutoffDate.tm_year = today.year - 1900;
  cutoffDate.tm_mon = today.month - 1;
  cutoffDate.tm_mday = today.day - 4;
  cutoffDate.tm_hour = 12;
  cutoffDate.tm_isdst = -1;
  const std::time_t cutoffTime = std::mktime(&cutoffDate);
  if (cutoffTime == static_cast<std::time_t>(-1) || localtime_r(&cutoffTime, &cutoffDate) == nullptr) return false;
  const int cutoffKey = (cutoffDate.tm_year + 1900) * 10000 + (cutoffDate.tm_mon + 1) * 100 + cutoffDate.tm_mday;

  HalFile directory = Storage.open(kCacheDirectory);
  if (!directory.isOpen() || !directory.isDirectory()) return false;

  bool success = true;
  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    char name[64] = {};
    const size_t nameLength = entry.getName(name, sizeof(name));
    entry.close();
    if (isDirectory || nameLength < 12) continue;

    const char* filename = std::strrchr(name, '/');
    filename = filename != nullptr ? filename + 1 : name;
    int dateKey = 0;
    if (!cacheFilenameDateKey(filename, dateKey) || dateKey >= cutoffKey) continue;

    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s", kCacheDirectory, filename);
    if (!Storage.remove(path)) {
      LOG_ERR("USCCB", "Could not remove expired cache %s", path);
      success = false;
    }
  }
  directory.close();
  return success;
}

FetchResult fetchDay(const Day& day) {
  if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
      ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
    LOG_ERR("USCCB", "Insufficient TLS heap: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return FetchResult::Failed;
  }
  if (!Storage.exists(kCacheDirectory) && !Storage.mkdir(kCacheDirectory)) return FetchResult::Failed;

  const std::string path = cachePath(day);
  const std::string textTempPath = path + ".part";
  const std::string rawTempPath = path + ".html.part";
  HalFile file = Storage.open(rawTempPath.c_str(), O_WRONLY | O_CREAT);
  if (!file.isOpen()) return FetchResult::Failed;

  size_t resumeOffset = file.size();
  if (resumeOffset > kMaxPageBytes) {
    file.close();
    if (!Storage.remove(rawTempPath.c_str())) return FetchResult::Failed;
    file = Storage.open(rawTempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!file.isOpen()) return FetchResult::Failed;
    resumeOffset = 0;
  }
  if (!file.seekSet(resumeOffset)) {
    file.close();
    return FetchResult::Failed;
  }

  char date[16];
  std::snprintf(date, sizeof(date), "%02d%02d%02d", day.month, day.day, day.year % 100);
  const std::string url = std::string("https://bible.usccb.org/bible/readings/") + date + ".cfm";
  WifiPowerSaveGuard powerSaveGuard;
  size_t written = resumeOffset;
  bool pageTooLarge = false;
  freeink::FetchOptions options;
  options.startOffset = resumeOffset;
  freeink::FetchSink sink;
  sink.write = [&](const uint8_t* data, const size_t size) {
    if (size > kMaxPageBytes - written) {
      pageTooLarge = true;
      return false;
    }
    if (file.write(data, size) != size) return false;
    written += size;
    return true;
  };
  sink.rewind = [&] {
    file.close();
    file = Storage.open(rawTempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    written = 0;
    return file.isOpen();
  };

  const freeink::FetchResult transfer = freeink::fetchResumable(
      url, options,
      [](freeink::SecureHttpClient& http, const bool) {
        http.setCACert(study::kBridgeCaRoots);
        http.setTimeout(kHttpTimeoutMs);
        http.setUserAgent("CrossPoint-Reader");
      },
      sink);
  file.flush();
  file.close();

  if (!transfer.complete || (transfer.status != 200 && transfer.status != 206)) {
    LOG_ERR("USCCB", "Fetch %s incomplete: HTTP=%d bytes=%u total=%u stopped=%d aborted=%d", date,
            transfer.status, static_cast<unsigned>(transfer.bytes), static_cast<unsigned>(transfer.total),
            transfer.stopped, transfer.aborted);
    if (pageTooLarge) Storage.remove(rawTempPath.c_str());
    return transfer.status == 404 ? FetchResult::NotPublished : FetchResult::Failed;
  }

  if (!parsePageFile(rawTempPath.c_str(), textTempPath.c_str())) {
    LOG_ERR("USCCB", "Could not parse completed page %s", date);
    Storage.remove(rawTempPath.c_str());
    return FetchResult::Failed;
  }
  if (!Storage.replaceFile(textTempPath.c_str(), path.c_str())) {
    Storage.remove(textTempPath.c_str());
    return FetchResult::Failed;
  }
  Storage.remove(rawTempPath.c_str());
  return FetchResult::Saved;
}

}  // namespace usccb
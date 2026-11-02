#include "TriviaPackStore.h"

#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kDirectory = "/trivia";
constexpr const char* kPackPath = "/trivia/pack.dat";
constexpr const char* kPackPartPath = "/trivia/pack.dat.part";
constexpr const char* kPackBackupPath = "/trivia/pack.dat.bak";
constexpr const char* kStatePath = "/trivia/pack.state";
constexpr const char* kStateTempPath = "/trivia/pack.state.tmp";
constexpr const char* kStateBackupPath = "/trivia/pack.state.bak";
constexpr const char* kMetaPath = "/trivia/pack.meta";
constexpr const char* kMetaTempPath = "/trivia/pack.meta.tmp";
constexpr const char* kMetaBackupPath = "/trivia/pack.meta.bak";
constexpr const char* kReportsPath = "/trivia/reports.dat";
constexpr const char* kReportsTempPath = "/trivia/reports.dat.tmp";
constexpr const char* kReportsBackupPath = "/trivia/reports.dat.bak";

bool matchesPackId(trivia::ByteSource& source, const char* id) {
  constexpr size_t kPackIdHexChars = 12;
  if (id == nullptr || std::strlen(id) != kPackIdHexChars) return false;
  for (size_t i = 0; i < kPackIdHexChars; ++i) {
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(1024);
  if (!buffer) {
    LOG_ERR("TRIVIA", "OOM: pack verification buffer");
    return false;
  }

  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  int result = mbedtls_sha256_starts(&hash, 0);
  for (uint32_t offset = 0; result == 0 && offset < source.size();) {
    const uint32_t chunk = source.size() - offset < 1024 ? source.size() - offset : 1024;
    if (!source.read(offset, buffer.get(), chunk)) {
      LOG_ERR("TRIVIA", "Cannot read question pack while verifying identity");
      result = -1;
      break;
    }
    result = mbedtls_sha256_update(&hash, buffer.get(), chunk);
    offset += chunk;
  }

  uint8_t digest[32] = {};
  if (result == 0) result = mbedtls_sha256_finish(&hash, digest);
  mbedtls_sha256_free(&hash);
  if (result != 0) {
    LOG_ERR("TRIVIA", "SHA-256 pack verification failed: %d", result);
    return false;
  }

  char actual[kPackIdHexChars + 1] = {};
  for (size_t i = 0; i < kPackIdHexChars / 2; ++i) {
    std::snprintf(actual + i * 2, sizeof(actual) - i * 2, "%02x", digest[i]);
  }
  if (std::strcmp(actual, id) != 0) {
    LOG_ERR("TRIVIA", "Question pack does not match its published identity");
    return false;
  }
  return true;
}

}  // namespace

TriviaPackStore::~TriviaPackStore() { close(); }

void TriviaPackStore::FileSource::attach(HalFile& file, const bool allowGrowth) {
  file_ = &file;
  size_ = static_cast<uint32_t>(file.size());
  allowGrowth_ = allowGrowth;
}

void TriviaPackStore::FileSource::detach() {
  file_ = nullptr;
  size_ = 0;
  allowGrowth_ = false;
}

bool TriviaPackStore::FileSource::read(const uint32_t offset, void* dst, const uint32_t length) {
  if (length == 0) return true;
  if (file_ == nullptr || !file_->isOpen() || offset > size_ || length > size_ - offset) return false;
  return file_->seekSet(offset) && file_->read(dst, length) == static_cast<int>(length);
}

bool TriviaPackStore::FileSource::write(const uint32_t offset, const void* src, const uint32_t length) {
  if (length == 0) return true;
  if (file_ == nullptr || !file_->isOpen() || offset > size_ || length > UINT32_MAX - offset) return false;
  const uint32_t end = offset + length;
  if (end > size_ && (!allowGrowth_ || offset != size_)) return false;
  if (!file_->seekSet(offset))
    return false;
  if (file_->write(src, length) != length) return false;
  if (end > size_) size_ = end;
  return true;
}

bool TriviaPackStore::FileSource::flush() {
  if (file_ == nullptr || !file_->isOpen()) return false;
  file_->flush();
  return true;
}

bool TriviaPackStore::removeIfPresent(const char* path) {
  return !Storage.exists(path) || Storage.remove(path);
}

bool TriviaPackStore::restorePackAndState() {
  bool ok = true;
  const bool restoringPack = Storage.exists(kPackBackupPath);
  if (restoringPack) {
    if (Storage.exists(kPackPath) && !Storage.remove(kPackPath)) {
      LOG_ERR("TRIVIA", "Cannot remove incomplete pack during recovery");
      ok = false;
    }
    if (!Storage.exists(kPackPath) && !Storage.rename(kPackBackupPath, kPackPath)) {
      LOG_ERR("TRIVIA", "Cannot restore the previous question pack");
      ok = false;
    }
  }
  if (Storage.exists(kStateBackupPath)) {
    if (restoringPack || !Storage.exists(kStatePath)) {
      if (Storage.exists(kStatePath) && !Storage.remove(kStatePath)) {
        LOG_ERR("TRIVIA", "Cannot remove incomplete question state during recovery");
        ok = false;
      } else if (!Storage.rename(kStateBackupPath, kStatePath)) {
        LOG_ERR("TRIVIA", "Cannot restore previous question state");
        ok = false;
      }
    } else if (!Storage.remove(kStateBackupPath)) {
      LOG_ERR("TRIVIA", "Cannot remove stale question state backup");
      ok = false;
    }
  }
  if (Storage.exists(kMetaBackupPath)) {
    if (restoringPack || !Storage.exists(kMetaPath)) {
      if (Storage.exists(kMetaPath) && !Storage.remove(kMetaPath)) {
        LOG_ERR("TRIVIA", "Cannot remove incomplete pack metadata during recovery");
        ok = false;
      } else if (!Storage.rename(kMetaBackupPath, kMetaPath)) {
        LOG_ERR("TRIVIA", "Cannot restore previous pack metadata");
        ok = false;
      }
    } else if (!Storage.remove(kMetaBackupPath)) {
      LOG_ERR("TRIVIA", "Cannot remove stale pack metadata backup");
      ok = false;
    }
  }
  return ok;
}

bool TriviaPackStore::restoreState() {
  if (!Storage.exists(kStateBackupPath)) return true;
  if (Storage.exists(kStatePath) && !Storage.remove(kStatePath)) {
    LOG_ERR("TRIVIA", "Cannot remove incomplete question state during recovery");
    return false;
  }
  if (!Storage.rename(kStateBackupPath, kStatePath)) {
    LOG_ERR("TRIVIA", "Cannot restore previous question state");
    return false;
  }
  return true;
}

bool TriviaPackStore::recoverFiles() {
  const bool hasPackBackup = Storage.exists(kPackBackupPath);
  const bool hasStateBackup = Storage.exists(kStateBackupPath);
  if (hasPackBackup && (!Storage.exists(kPackPath) || !Storage.exists(kStatePath))) {
    if (!restorePackAndState()) return false;
  } else if (hasStateBackup && !Storage.exists(kStatePath)) {
    if (!restoreState()) return false;
  } else {
    if (hasPackBackup && !removeIfPresent(kMetaBackupPath)) {
      LOG_ERR("TRIVIA", "Cannot remove completed pack metadata backup");
      return false;
    }
    if (Storage.exists(kMetaBackupPath) && !hasPackBackup && !removeIfPresent(kMetaBackupPath)) {
      LOG_ERR("TRIVIA", "Cannot remove stale pack metadata backup");
      return false;
    }
    if (hasStateBackup && !removeIfPresent(kStateBackupPath)) {
      LOG_ERR("TRIVIA", "Cannot remove completed state backup");
      return false;
    }
    if (hasPackBackup && !removeIfPresent(kPackBackupPath)) {
      LOG_ERR("TRIVIA", "Cannot remove completed pack backup");
      return false;
    }
  }

  if (Storage.exists(kReportsBackupPath)) {
    if (Storage.exists(kReportsPath)) {
      if (!removeIfPresent(kReportsBackupPath)) {
        LOG_ERR("TRIVIA", "Cannot remove completed report queue backup");
        return false;
      }
    } else if (!Storage.rename(kReportsBackupPath, kReportsPath)) {
      LOG_ERR("TRIVIA", "Cannot restore previous report queue");
      return false;
    }
  }
  if (!removeIfPresent(kStateTempPath) || !removeIfPresent(kMetaTempPath) || !removeIfPresent(kPackPartPath) ||
      !removeIfPresent(kReportsTempPath)) {
    LOG_ERR("TRIVIA", "Cannot remove an incomplete download or state file");
    return false;
  }
  return true;
}

bool TriviaPackStore::writeEmptyState(const uint32_t count) {
  HalFile tempFile;
  if (!Storage.openFileForWrite("TRIVIA", kStateTempPath, tempFile)) {
    LOG_ERR("TRIVIA", "Cannot create temporary question state");
    return false;
  }

  uint8_t zeros[128] = {};
  for (uint32_t written = 0; written < count;) {
    const uint32_t chunk = (count - written) < sizeof(zeros) ? (count - written) : sizeof(zeros);
    if (tempFile.write(zeros, chunk) != chunk) {
      LOG_ERR("TRIVIA", "Short write creating question state");
      if (tempFile.close()) {
        removeIfPresent(kStateTempPath);
      } else {
        LOG_ERR("TRIVIA", "Cannot close partial temporary question state");
      }
      return false;
    }
    written += chunk;
  }
  tempFile.flush();
  if (!tempFile.close()) {
    LOG_ERR("TRIVIA", "Cannot close temporary question state");
    return false;
  }
  return true;
}

bool TriviaPackStore::ensureState(const uint32_t count) {
  stateFile_ = Storage.open(kStatePath, O_RDWR);
  if (stateFile_.isOpen() && stateFile_.size() == count) {
    stateSource_.attach(stateFile_);
    if (state_.open(stateSource_, count)) return true;
    LOG_ERR("TRIVIA", "Cannot read question state");
    if (!stateFile_.close()) {
      LOG_ERR("TRIVIA", "Cannot close invalid question state");
      return false;
    }
    return false;
  }
  if (stateFile_.isOpen() && !stateFile_.close()) {
    LOG_ERR("TRIVIA", "Cannot close old question state");
    return false;
  }

  if (!writeEmptyState(count)) return false;
  const bool hadState = Storage.exists(kStatePath);
  if (hadState && !Storage.rename(kStatePath, kStateBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot preserve old question state");
    removeIfPresent(kStateTempPath);
    return false;
  }
  if (!Storage.rename(kStateTempPath, kStatePath)) {
    LOG_ERR("TRIVIA", "Cannot install question state");
    if (hadState && !Storage.rename(kStateBackupPath, kStatePath)) {
      LOG_ERR("TRIVIA", "Cannot restore old question state");
    }
    return false;
  }
  if (hadState && !removeIfPresent(kStateBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot remove old question state");
    return false;
  }

  stateFile_ = Storage.open(kStatePath, O_RDWR);
  if (!stateFile_.isOpen()) {
    LOG_ERR("TRIVIA", "Cannot open question state");
    return false;
  }
  stateSource_.attach(stateFile_);
  if (!state_.open(stateSource_, count)) {
    LOG_ERR("TRIVIA", "Cannot validate question state");
    if (!stateFile_.close()) LOG_ERR("TRIVIA", "Cannot close invalid question state");
    return false;
  }
  return true;
}

bool TriviaPackStore::loadPackMeta() {
  packMeta_ = {};
  HalFile file = Storage.open(kMetaPath, O_RDONLY);
  if (!file.isOpen()) {
    const bool exists = Storage.exists(kMetaPath);
    if (exists) LOG_ERR("TRIVIA", "Cannot open pack metadata");
    return !exists;
  }

  constexpr size_t kMetaBufferBytes = 128;
  const size_t size = file.size();
  char text[kMetaBufferBytes] = {};
  if (size == 0 || size >= sizeof(text)) {
    LOG_ERR("TRIVIA", "Invalid pack metadata size");
    if (!file.close()) LOG_ERR("TRIVIA", "Cannot close invalid pack metadata");
    return false;
  }
  const int bytesRead = file.read(text, size);
  if (!file.close()) {
    LOG_ERR("TRIVIA", "Cannot close pack metadata");
    return false;
  }
  if (bytesRead != static_cast<int>(size)) {
    LOG_ERR("TRIVIA", "Cannot read complete pack metadata");
    return false;
  }

  packMeta_ = trivia::parseMeta(text, size, pack_.count(), packSource_.size());
  if (packMeta_.valid && !matchesPackId(packSource_, packMeta_.id)) packMeta_ = {};
  if (!packMeta_.valid) LOG_INF("TRIVIA", "Pack identity is unknown; reports remain disabled");
  return packMeta_.valid;
}

bool TriviaPackStore::closeReportQueue() {
  const bool closed = !reportFile_.isOpen() || reportFile_.close();
  if (!closed) {
    LOG_ERR("TRIVIA", "Cannot close report queue");
    return false;
  }
  reports_ = {};
  reportSource_.detach();
  reportQueueState_ = trivia::QueueOpen::Unusable;
  return true;
}

trivia::QueueOpen TriviaPackStore::openReportQueue() {
  if (!closeReportQueue()) {
    reportQueueState_ = trivia::QueueOpen::Unusable;
    return trivia::QueueOpen::Unusable;
  }
  if (!packMeta_.valid || !pack_.isOpen()) return trivia::QueueOpen::Unusable;

  if (!Storage.exists(kReportsPath)) {
    HalFile fresh;
    if (!Storage.openFileForWrite("TRIVIA", kReportsPath, fresh)) {
      LOG_ERR("TRIVIA", "Cannot create report queue");
      return trivia::QueueOpen::Unusable;
    }
    if (!fresh.close()) {
      LOG_ERR("TRIVIA", "Cannot close new report queue");
      return trivia::QueueOpen::Unusable;
    }
  }

  reportFile_ = Storage.open(kReportsPath, O_RDWR);
  if (!reportFile_.isOpen()) {
    LOG_ERR("TRIVIA", "Cannot open report queue");
    return trivia::QueueOpen::Unusable;
  }
  constexpr size_t kMaxReportFileBytes =
      trivia::kReportHeaderBytes + trivia::kMaxQueuedReports * trivia::kReportEntryBytes;
  if (reportFile_.size() > kMaxReportFileBytes) {
    LOG_ERR("TRIVIA", "Report queue exceeds its maximum size");
    if (!closeReportQueue()) return trivia::QueueOpen::Unusable;
    return trivia::QueueOpen::Unusable;
  }

  reportSource_.attach(reportFile_, true);
  reportQueueState_ = reports_.open(reportSource_, packMeta_.id, pack_.count());
  if (reportQueueState_ == trivia::QueueOpen::Unusable) {
    LOG_ERR("TRIVIA", "Report queue is not readable");
    if (!closeReportQueue()) return trivia::QueueOpen::Unusable;
  }
  return reportQueueState_;
}

bool TriviaPackStore::resetReportQueue() {
  if (!pack_.isOpen() || !packMeta_.valid || reports_.pending() != 0 ||
      (reportQueueState_ != trivia::QueueOpen::Ready && reportQueueState_ != trivia::QueueOpen::Started &&
       reportQueueState_ != trivia::QueueOpen::Foreign)) {
    LOG_ERR("TRIVIA", "Cannot reset a report queue with pending or invalid reports");
    return false;
  }
  if (!closeReportQueue() || !removeIfPresent(kReportsTempPath) || !removeIfPresent(kReportsBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot prepare report queue replacement");
    return false;
  }

  HalFile freshFile;
  if (!Storage.openFileForWrite("TRIVIA", kReportsTempPath, freshFile)) {
    LOG_ERR("TRIVIA", "Cannot create replacement report queue");
    return false;
  }
  FileSource freshSource;
  freshSource.attach(freshFile, true);
  trivia::ReportQueue freshQueue;
  const trivia::QueueOpen freshResult = freshQueue.open(freshSource, packMeta_.id, pack_.count());
  if (freshResult != trivia::QueueOpen::Started) {
    LOG_ERR("TRIVIA", "Cannot initialize replacement report queue");
    if (!freshFile.close()) LOG_ERR("TRIVIA", "Cannot close invalid replacement report queue");
    freshSource.detach();
    removeIfPresent(kReportsTempPath);
    return false;
  }
  if (!freshFile.close()) {
    LOG_ERR("TRIVIA", "Cannot close replacement report queue");
    freshSource.detach();
    return false;
  }
  freshSource.detach();

  const bool hadQueue = Storage.exists(kReportsPath);
  if (hadQueue && !Storage.rename(kReportsPath, kReportsBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot preserve old report queue");
    removeIfPresent(kReportsTempPath);
    return false;
  }
  if (!Storage.rename(kReportsTempPath, kReportsPath)) {
    LOG_ERR("TRIVIA", "Cannot install replacement report queue");
    if (hadQueue && !Storage.rename(kReportsBackupPath, kReportsPath))
      LOG_ERR("TRIVIA", "Cannot restore old report queue");
    return false;
  }
  if (hadQueue && !removeIfPresent(kReportsBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot remove old report queue backup");
    return false;
  }
  return openReportQueue() != trivia::QueueOpen::Unusable;
}

bool TriviaPackStore::open() {
  if (!close()) return false;
  if (!recoverFiles()) return false;
  if (!Storage.openFileForRead("TRIVIA", kPackPath, packFile_)) return false;
  packSource_.attach(packFile_);
  if (!pack_.open(packSource_)) {
    LOG_ERR("TRIVIA", "Question pack has an invalid format");
    if (!close()) {
      LOG_ERR("TRIVIA", "Cannot close invalid question pack");
      return false;
    }
    return false;
  }
  if (!ensureState(pack_.count())) {
    if (!close()) LOG_ERR("TRIVIA", "Cannot close question files after state failure");
    return false;
  }
  loadPackMeta();
  openReportQueue();
  return true;
}

bool TriviaPackStore::adoptManifest(const trivia::PackManifest& manifest) {
  if (!manifest.valid || !pack_.isOpen() || manifest.count != pack_.count() ||
      manifest.bytes != packSource_.size() || !matchesPackId(packSource_, manifest.id)) {
    LOG_INF("TRIVIA", "Manifest does not describe the open question pack");
    return false;
  }
  return writePackMeta(manifest);
}

bool TriviaPackStore::writePackMeta(const trivia::PackManifest& manifest) {
  if (!manifest.valid || !pack_.isOpen() || manifest.count != pack_.count() ||
      manifest.bytes != packSource_.size()) {
    LOG_INF("TRIVIA", "Manifest does not describe the open question pack");
    return false;
  }
  if (!closeReportQueue()) return false;

  char text[128] = {};
  const size_t length = trivia::formatMeta(text, sizeof(text), manifest.id, manifest.count, manifest.bytes);
  if (length == 0) {
    LOG_ERR("TRIVIA", "Cannot format pack metadata");
    return false;
  }
  if (!removeIfPresent(kMetaTempPath)) {
    LOG_ERR("TRIVIA", "Cannot remove stale temporary pack metadata");
    return false;
  }

  HalFile tempFile;
  if (!Storage.openFileForWrite("TRIVIA", kMetaTempPath, tempFile)) {
    LOG_ERR("TRIVIA", "Cannot create temporary pack metadata");
    return false;
  }
  if (tempFile.write(reinterpret_cast<const uint8_t*>(text), length) != length) {
    LOG_ERR("TRIVIA", "Short write saving pack metadata");
    if (!tempFile.close()) {
      LOG_ERR("TRIVIA", "Cannot close temporary pack metadata");
      return false;
    }
    if (!removeIfPresent(kMetaTempPath)) LOG_ERR("TRIVIA", "Cannot remove partial pack metadata");
    return false;
  }
  tempFile.flush();
  if (!tempFile.close()) {
    LOG_ERR("TRIVIA", "Cannot close temporary pack metadata");
    return false;
  }

  if (!removeIfPresent(kMetaBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot remove stale pack metadata backup");
    return false;
  }
  const bool hadMeta = Storage.exists(kMetaPath);
  if (hadMeta && !Storage.rename(kMetaPath, kMetaBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot preserve existing pack metadata");
    if (!removeIfPresent(kMetaTempPath)) LOG_ERR("TRIVIA", "Cannot remove temporary pack metadata");
    return false;
  }
  if (!Storage.rename(kMetaTempPath, kMetaPath)) {
    LOG_ERR("TRIVIA", "Cannot install pack metadata");
    if (hadMeta && !Storage.rename(kMetaBackupPath, kMetaPath))
      LOG_ERR("TRIVIA", "Cannot restore existing pack metadata");
    return false;
  }
  if (hadMeta && !removeIfPresent(kMetaBackupPath)) {
    LOG_ERR("TRIVIA", "Cannot remove old pack metadata");
  }

  std::memcpy(packMeta_.id, manifest.id, sizeof(packMeta_.id));
  packMeta_.count = manifest.count;
  packMeta_.bytes = manifest.bytes;
  packMeta_.valid = true;
  openReportQueue();
  return packMeta_.valid;
}

bool TriviaPackStore::installDownloadedPack(const trivia::PackManifest& manifest) {
  HalFile candidateFile;
  if (!Storage.openFileForRead("TRIVIA", kPackPartPath, candidateFile)) {
    LOG_ERR("TRIVIA", "Cannot open downloaded question pack");
    return false;
  }
  FileSource candidateSource;
  candidateSource.attach(candidateFile);
  trivia::Pack candidatePack;
  if (!candidatePack.open(candidateSource)) {
    LOG_ERR("TRIVIA", "Downloaded question pack has an invalid format");
    if (!candidateFile.close()) {
      LOG_ERR("TRIVIA", "Cannot close downloaded question pack");
      return false;
    }
    removeIfPresent(kPackPartPath);
    return false;
  }
  const uint32_t count = candidatePack.count();
  if (!manifest.valid || manifest.count != count || manifest.bytes != candidateSource.size() ||
      !matchesPackId(candidateSource, manifest.id)) {
    LOG_ERR("TRIVIA", "Downloaded question pack does not match the published manifest");
    if (!candidateFile.close()) LOG_ERR("TRIVIA", "Cannot close unverified question pack");
    if (!removeIfPresent(kPackPartPath)) LOG_ERR("TRIVIA", "Cannot remove unverified question pack");
    return false;
  }
  if (!candidateFile.close()) {
    LOG_ERR("TRIVIA", "Cannot close downloaded question pack");
    return false;
  }

  if (!writeEmptyState(count)) return false;
  if (!close()) {
    LOG_ERR("TRIVIA", "Cannot close current pack before installing update");
    return false;
  }

  const bool hadPack = Storage.exists(kPackPath);
  const bool hadState = Storage.exists(kStatePath);
  const bool hadMeta = Storage.exists(kMetaPath);
  if ((hadPack && !Storage.rename(kPackPath, kPackBackupPath)) ||
      (hadState && !Storage.rename(kStatePath, kStateBackupPath)) ||
      (hadMeta && !Storage.rename(kMetaPath, kMetaBackupPath))) {
    LOG_ERR("TRIVIA", "Cannot preserve the current pack and progress");
    restorePackAndState();
    removeIfPresent(kStateTempPath);
    removeIfPresent(kPackPartPath);
    return false;
  }
  if (!Storage.rename(kPackPartPath, kPackPath) || !Storage.rename(kStateTempPath, kStatePath)) {
    LOG_ERR("TRIVIA", "Cannot install downloaded pack and progress");
    restorePackAndState();
    removeIfPresent(kStateTempPath);
    removeIfPresent(kPackPartPath);
    return false;
  }

  if (!removeIfPresent(kMetaBackupPath) || !removeIfPresent(kStateBackupPath) ||
      !removeIfPresent(kPackBackupPath)) {
    LOG_ERR("TRIVIA", "New pack installed but old backups could not be removed");
    return false;
  }
  if (!open()) return false;
  return writePackMeta(manifest);
}

bool TriviaPackStore::close() {
  const bool reportsClosed = closeReportQueue();
  const bool packClosed = !packFile_.isOpen() || packFile_.close();
  const bool stateClosed = !stateFile_.isOpen() || stateFile_.close();
  if (!reportsClosed) LOG_ERR("TRIVIA", "Cannot close report queue before closing Trivia store");
  if (!packClosed) LOG_ERR("TRIVIA", "Cannot close question pack");
  if (!stateClosed) LOG_ERR("TRIVIA", "Cannot close question state");
  if (packClosed) {
    packSource_.detach();
    pack_.open(packSource_);
  }
  if (stateClosed) {
    stateSource_.detach();
    state_.open(stateSource_, 0);
  }
  if (reportsClosed && packClosed && stateClosed) packMeta_ = {};
  return reportsClosed && packClosed && stateClosed;
}

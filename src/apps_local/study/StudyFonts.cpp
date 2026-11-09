#include "StudyFonts.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFontRegistry.h>

#include <cstdio>
#include <cstring>

#include "StudyText.h"

namespace study {

namespace {

// The order Mario's HSK template lists them in `var fonts = [...]`.
constexpr const char* kFamilies[StudyFonts::kFamilyCount] = {
    "SimSun", "SimHei", "MicrosoftYaHei", "KaiTi", "FangSong", "Custom",
};

constexpr const char* kLegacyFontRoot = "/study/fonts";

bool largestStudyFontSizes(const SdCardFontFamilyInfo& family, uint8_t (&largest)[4]) {
  size_t count = 0;
  for (const auto& file : family.files) {
    const uint8_t size = file.pointSize;
    if (size == 0) continue;

    size_t position = 0;
    while (position < count && largest[position] < size) ++position;
    if (position < count && largest[position] == size) continue;
    if (position >= 4) continue;

    if (count < 4) ++count;
    for (size_t i = count - 1; i > position; --i) largest[i] = largest[i - 1];
    largest[position] = size;
  }
  return count == 4;
}

// Build the family descriptor by hand rather than going through
// SdCardFontRegistry. The registry only scans /.fonts and /fonts, and putting
// these there would list them as reading fonts -- see the header for why that
// is a trap. SdCardFontFamilyInfo is a plain struct, so this costs nothing.
SdCardFontFamilyInfo describe(const char* root, const char* family, const uint8_t pointSize) {
  SdCardFontFamilyInfo info;
  info.name = family;
  info.root = root;
  info.files.push_back(SdCardFontFileInfo{nullptr, pointSize, 0});
  return info;
}

bool anyFamilyUnder(const char* root) {
  char path[128];
  for (int i = 0; i < StudyFonts::kFamilyCount; ++i) {
    std::snprintf(path, sizeof(path), "%s/%s/%s_%u.cpfont", root, kFamilies[i], kFamilies[i], kHeadwordPointSize);
    if (Storage.exists(path)) return true;
  }
  return false;
}

}  // namespace

void StudyFonts::setRoot(const char* deckDir) {
  std::snprintf(root_, sizeof(root_), "%s/fonts", deckDir);
  if (!anyFamilyUnder(root_) && anyFamilyUnder(kLegacyFontRoot)) {
    std::snprintf(root_, sizeof(root_), "%s", kLegacyFontRoot);
  }
}

void StudyFonts::probe() {
  presentCount_ = 0;
  char path[128];
  for (int i = 0; i < kFamilyCount; ++i) {
    // The headword cut is the expensive one, so its presence is the test; a
    // family with only the sentence cut is half-installed and load() would
    // refuse it anyway.
    std::snprintf(path, sizeof(path), "%s/%s/%s_%u.cpfont", root_, kFamilies[i], kFamilies[i], kHeadwordPointSize);
    if (Storage.exists(path)) present_[presentCount_++] = i;
  }
  LOG_INF("STUDY", "%d of %d font families under %s", presentCount_, kFamilyCount, root_);
}

const char* StudyFonts::familyName(const int index) {
  if (index < 0 || index >= kFamilyCount) return "";
  return kFamilies[index];
}

bool StudyFonts::load(GfxRenderer& renderer, const int familyIndex) {
  if (familyIndex < 0 || familyIndex >= kFamilyCount) return false;
  if (familyIndex == familyIndex_ && loaded()) return true;

  unloadDeckFonts(renderer);
  const char* family = kFamilies[familyIndex];

  const int headword = manager_.loadFamilyExtraSize(describe(root_, family, kHeadwordPointSize), renderer,
                                                    kHeadwordPointSize, 0);
  if (headword != 0) deckFontIds_[0] = headword;
  const int sentence = manager_.loadFamilyExtraSize(describe(root_, family, kSentencePointSize), renderer,
                                                    kSentencePointSize, 0);
  if (sentence != 0) deckFontIds_[1] = sentence;
  // Optional: only a deck with furigana has one built. A zero here is the
  // ordinary answer for every deck that is not Japanese, so it is not part of
  // the failure test below.
  char rubyPath[128];
  std::snprintf(rubyPath, sizeof(rubyPath), "%s/%s/%s_%u.cpfont", root_, family, family, kRubyPointSize);
  const int ruby = Storage.exists(rubyPath)
                       ? manager_.loadFamilyExtraSize(describe(root_, family, kRubyPointSize), renderer,
                                                      kRubyPointSize, 0)
                       : 0;
  if (ruby != 0) deckFontIds_[2] = ruby;
  if (headword == 0 || sentence == 0) {
    // Debug, not error: an absent family is now a normal answer, because
    // loadPreferred walks past it to one that is there. Only having none of
    // them is worth an error, and loadPreferred raises that itself.
    LOG_DBG("STUDY", "Font family %s not on the card under %s (headword=%d sentence=%d)", family, root_, headword,
            sentence);
    unloadDeckFonts(renderer);
    return false;
  }

  headwordFontId_ = headword;
  sentenceFontId_ = sentence;
  rubyFontId_ = ruby;
  familyIndex_ = familyIndex;
  LOG_DBG("STUDY", "Loaded %s: headword id=%d sentence id=%d ruby id=%d", family, headword, sentence, ruby);
  return true;
}

int StudyFonts::loadPreferred(GfxRenderer& renderer, const int preferred) {
  const int first = (preferred < 0 || preferred >= kFamilyCount) ? 0 : preferred;
  for (int i = 0; i < kFamilyCount; ++i) {
    const int index = (first + i) % kFamilyCount;
    if (load(renderer, index)) return index;
  }
  LOG_ERR("STUDY", "No CJK family under %s; hanzi will not draw", root_);
  return -1;
}

void StudyFonts::unload(GfxRenderer& renderer) {
  unloadDeckFonts(renderer);
  unloadIndexFonts(renderer);
}

void StudyFonts::unloadDeckFonts(GfxRenderer& renderer) {
  for (int& fontId : deckFontIds_) {
    if (fontId != 0) manager_.unloadFont(renderer, fontId);
    fontId = 0;
  }
  headwordFontId_ = 0;
  sentenceFontId_ = 0;
  rubyFontId_ = 0;
  familyIndex_ = -1;
}

void StudyFonts::loadIndexFonts(GfxRenderer& renderer, const SdCardFontRegistry& registry, const char* familyName,
                                const char* const* cardText, const size_t textCount) {
  const auto* family = familyName != nullptr && familyName[0] != '\0' ? registry.findFamily(familyName) : nullptr;
  uint8_t largestSizes[4] = {};
  const bool familyAvailable = family != nullptr && !family->vector && largestStudyFontSizes(*family, largestSizes);
  const bool familyNameFits = familyName != nullptr && std::strlen(familyName) < sizeof(detailsFont_.family);

  if (!familyAvailable || !familyNameFits) {
    if (detailsFont_.fontId != 0) manager_.unloadFont(renderer, detailsFont_.fontId);
    detailsFont_ = IndexFont{};
  } else if (detailsFont_.fontId == 0 || std::strcmp(detailsFont_.family, familyName) != 0) {
    if (detailsFont_.fontId != 0) manager_.unloadFont(renderer, detailsFont_.fontId);
    detailsFont_ = IndexFont{};
    detailsFont_.fontId = manager_.loadFamilyExtraSize(*family, renderer, largestSizes[0], 0);
    if (detailsFont_.fontId != 0) std::strcpy(detailsFont_.family, familyName);
  }

  for (int section = 0; section < kIndexSectionCount; ++section) {
    if (!familyAvailable || !familyNameFits) {
      if (indexFonts_[section].fontId != 0) manager_.unloadFont(renderer, indexFonts_[section].fontId);
      indexFonts_[section] = IndexFont{};
      continue;
    }
    if (indexFonts_[section].fontId != 0 && std::strcmp(indexFonts_[section].family, familyName) == 0) continue;

    if (indexFonts_[section].fontId != 0) manager_.unloadFont(renderer, indexFonts_[section].fontId);
    indexFonts_[section] = IndexFont{};

    // The family sizes are ordered ascending here: skip Small and map the
    // remaining cuts to Medium, Large, and Extra Large.
    const uint8_t pointSize = largestSizes[section + 1];

    const int fontId = manager_.loadFamilyExtraSize(*family, renderer, pointSize, 0);
    if (fontId == 0) {
      LOG_DBG("STUDY", "Failed to load Index font %s at %upt", familyName, pointSize);
      continue;
    }
    indexFonts_[section].fontId = fontId;
    std::strcpy(indexFonts_[section].family, familyName);
  }

  FontCacheManager* cache = renderer.getFontCacheManager();
  if (cache != nullptr && cardText != nullptr) {
    if (detailsFont_.fontId != 0) {
      for (size_t i = 0; i < textCount; ++i) {
        if (cardText[i] != nullptr && *cardText[i] != '\0') cache->prewarmCache(detailsFont_.fontId, cardText[i]);
      }
    }
    for (const auto& font : indexFonts_) {
      if (font.fontId == 0) continue;
      for (size_t i = 0; i < textCount; ++i) {
        if (cardText[i] != nullptr && *cardText[i] != '\0') cache->prewarmCache(font.fontId, cardText[i]);
      }
    }
  }
}

void StudyFonts::unloadIndexFonts(GfxRenderer& renderer) {
  if (detailsFont_.fontId != 0) manager_.unloadFont(renderer, detailsFont_.fontId);
  detailsFont_ = IndexFont{};
  for (auto& font : indexFonts_) {
    if (font.fontId != 0) manager_.unloadFont(renderer, font.fontId);
    font = IndexFont{};
  }
}

void StudyFonts::prewarm(GfxRenderer& renderer, const char* headword, const char* sentence) const {
  FontCacheManager* cache = renderer.getFontCacheManager();
  if (cache == nullptr || !loaded()) return;
  // The headword also appears inside its own example sentence, so both cuts
  // want it. Two calls, one per size, because the cache is keyed by font id.
  if (headword != nullptr && *headword != '\0') {
    cache->prewarmCache(headwordFontId_, headword);
  }
  if (sentence != nullptr && *sentence != '\0') {
    cache->prewarmCache(sentenceFontId_, sentence);
  }
}

void StudyFonts::prewarmRuby(GfxRenderer& renderer, const char* text) const {
  FontCacheManager* cache = renderer.getFontCacheManager();
  if (cache == nullptr || rubyFontId_ == 0 || text == nullptr || *text == '\0') return;
  // The readings only. Handing the whole encoded string over would ask the
  // ruby cut for every kanji in the sentence, none of which it was built
  // with, and a prewarm of glyphs that are not there is a seek per glyph for
  // nothing.
  // The readings on one card, not the card. A sentence of furigana is a few
  // dozen kana; this is several times that and it is a stack buffer, which is
  // the one place in this app where a kilobyte is not free.
  static constexpr int kMaxReadingBytes = 256;
  char readings[kMaxReadingBytes];
  int length = 0;
  forEachRubySegment(text, [&](const RubySegment& segment) {
    if (segment.ruby == nullptr) return;
    if (length + segment.rubyBytes >= static_cast<int>(sizeof(readings))) return;
    std::memcpy(readings + length, segment.ruby, static_cast<size_t>(segment.rubyBytes));
    length += segment.rubyBytes;
  });
  readings[length] = '\0';
  if (length > 0) cache->prewarmCache(rubyFontId_, readings);
}

}  // namespace study

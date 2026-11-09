#pragma once

#include <cstddef>
#include <ctime>
#include <string>

namespace usccb {

inline constexpr size_t kWindowSize = 7;

struct Day {
  int year = 0;
  int month = 0;
  int day = 0;
};

enum class FetchResult { Saved, NotPublished, Failed };

bool makeWindow(std::time_t now, Day out[kWindowSize]);
std::string cachePath(const Day& day);
bool isCached(const Day& day);
bool pruneExpiredCache(const Day& today);
FetchResult fetchDay(const Day& day);

}  // namespace usccb
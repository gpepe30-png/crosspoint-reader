#pragma once

#include <cstddef>

#include "TriviaReport.h"

namespace trivia {

bool fetchPublishedManifest(PackManifest& manifest);
int submitReportBatch(const char* body, size_t length);

}  // namespace trivia

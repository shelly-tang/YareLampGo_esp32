#pragma once

#include <string.h>

namespace ClipValidationScope {

inline bool isActiveSyncForClip(const char *syncClipId,
                                const char *candidateClipId,
                                bool syncInProgress) {
  if (!syncInProgress || !syncClipId || !candidateClipId || !syncClipId[0]) {
    return false;
  }
  return strcmp(syncClipId, candidateClipId) == 0;
}

inline bool shouldUseSyncExpectations(const char *syncClipId,
                                      const char *candidateClipId,
                                      bool syncInProgress,
                                      bool validatingStagedUpload) {
  return validatingStagedUpload &&
         isActiveSyncForClip(syncClipId, candidateClipId, syncInProgress);
}

}  // namespace ClipValidationScope

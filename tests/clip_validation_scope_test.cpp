#include <cassert>

#include "../ESP32_C6_LCD_1_47_UART/clip_validation_scope.h"

int main() {
  using ClipValidationScope::isActiveSyncForClip;
  using ClipValidationScope::shouldUseSyncExpectations;

  assert(isActiveSyncForClip("itachi-eyes", "itachi-eyes", true));
  assert(!isActiveSyncForClip("itachi-eyes", "cat-eyes", true));
  assert(!isActiveSyncForClip("itachi-eyes", "itachi-eyes", false));

  // Commit validates the staged payload against metadata for the same clip.
  assert(shouldUseSyncExpectations("itachi-eyes", "itachi-eyes", true, true));

  // The regression: after syncing Itachi, playing Cat must not reuse Itachi's
  // size, SHA, or sync error state.
  assert(!shouldUseSyncExpectations("itachi-eyes", "cat-eyes", true, false));
  assert(!shouldUseSyncExpectations("itachi-eyes", "cat-eyes", true, true));

  // A failed replacement must not poison the previously installed version of
  // the same clip when it is opened for playback.
  assert(!shouldUseSyncExpectations("itachi-eyes", "itachi-eyes", true, false));

  // A staged file cannot be committed without a matching active begin.
  assert(!shouldUseSyncExpectations("itachi-eyes", "itachi-eyes", false, true));
  assert(!shouldUseSyncExpectations("", "cat-eyes", true, true));
  assert(!shouldUseSyncExpectations(nullptr, "cat-eyes", true, true));
  assert(!shouldUseSyncExpectations("itachi-eyes", nullptr, true, true));
  return 0;
}

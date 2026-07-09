#pragma once

#include <stddef.h>

namespace DisplayLink {

bool begin();
void loop();
void sendStatus(const char *status, const char *detail = nullptr);
void sendExpression(const char *expression);
void sendClipBegin(const char *clipId, int fps, int frameCount, int durationMs, size_t lcdBytes, const char *lcdSha256);
void sendClipChunk(const char *clipId, size_t offset, const char *hexData);
void sendClipCommit(const char *clipId);
void sendClipPlay(const char *clipId);

}  // namespace DisplayLink

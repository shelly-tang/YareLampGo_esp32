#pragma once

#include <stddef.h>
#include <stdint.h>

namespace DisplayLink {

bool begin();
void loop();
void sendStatus(const char *status, const char *detail = nullptr);
void sendExpression(const char *expression);
const char *lastClipError();
bool sendClipBegin(const char *clipId, int fps, int frameCount, int durationMs, size_t lcdBytes, const char *lcdSha256);
bool sendClipChunk(const char *clipId, size_t offset, const char *hexData);
bool sendClipChunkBytes(const char *clipId, size_t offset, const uint8_t *data, size_t len);
bool sendClipCommit(const char *clipId, size_t expectedOffset = SIZE_MAX);
void sendClipPlay(const char *clipId, bool loop = true);
void sendClipStop();
size_t c6FsTotalBytes();
size_t c6FsUsedBytes();
size_t c6InstalledBytes();
int c6InstalledCount();
int c6MaxClipCount();
size_t c6InstalledBudgetBytes();

}  // namespace DisplayLink

// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ExpressionClips {

bool begin();
const char *lastError();

bool beginSync(const char *clipId,
               const char *expression,
               int fps,
               int frameCount,
               int durationMs,
               size_t lcdBytes,
               const char *lcdSha256);
bool appendChunk(const char *clipId, const char *target, size_t offset, const char *hexData);
bool commitSync(const char *clipId);
bool removeClip(const char *clipId);

bool lcdPath(const char *clipId, char *out, size_t outLen);
bool manifestPath(const char *clipId, char *out, size_t outLen);

}  // namespace ExpressionClips

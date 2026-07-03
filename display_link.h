#pragma once

namespace DisplayLink {

bool begin();
void loop();
void sendStatus(const char *status, const char *detail = nullptr);
void sendExpression(const char *expression);

}  // namespace DisplayLink

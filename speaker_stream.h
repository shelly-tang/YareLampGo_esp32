#ifndef LAMPGO_SPEAKER_STREAM_H
#define LAMPGO_SPEAKER_STREAM_H

#include "esp_http_server.h"

namespace SpeakerStream {

bool begin();
void stop();
bool isRunning();
bool startPlaybackTask();
bool registerWsHandler(httpd_handle_t server);
size_t readReference(int16_t *out, size_t samples);
void clearReference();
void setVolume(float volume);
float getVolume();

}

#endif

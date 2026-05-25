#ifndef LAMPGO_SPEAKER_STREAM_H
#define LAMPGO_SPEAKER_STREAM_H

#include <stdint.h>
#include "esp_http_server.h"

namespace SpeakerStream {

bool begin();
bool isRunning();
bool registerWsHandler(httpd_handle_t server);
void closeClients();
size_t readReference(int16_t *out, size_t samples);
void clearReference();
void setVolume(float volume);
float getVolume();
uint32_t queuedPackets();
uint32_t packetsQueued();
uint32_t packetsDropped();
uint32_t underruns();
uint32_t shortWrites();

}

#endif

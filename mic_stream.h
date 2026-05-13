#ifndef LAMPGO_MIC_STREAM_H
#define LAMPGO_MIC_STREAM_H

#include <stdint.h>

#include "esp_http_server.h"

namespace MicStream {

bool begin();
void stop();
bool isRunning();
bool registerWsHandler(httpd_handle_t server);
bool isAecReady();
int clientCount();
uint32_t bytesRead();
uint32_t framesSent();

}

#endif

#ifndef LAMPGO_MIC_STREAM_H
#define LAMPGO_MIC_STREAM_H

#include "esp_http_server.h"

namespace MicStream {

bool begin();
void stop();
bool isRunning();
bool registerWsHandler(httpd_handle_t server);

}

#endif

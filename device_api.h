#ifndef LAMPGO_DEVICE_API_H
#define LAMPGO_DEVICE_API_H

#include "esp_http_server.h"

namespace DeviceApi {

bool registerHandlers(httpd_handle_t server);

}

#endif

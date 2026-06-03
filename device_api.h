// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LAMPGO_DEVICE_API_H
#define LAMPGO_DEVICE_API_H

#include "esp_http_server.h"

namespace DeviceApi {

bool registerHandlers(httpd_handle_t server);

}

#endif

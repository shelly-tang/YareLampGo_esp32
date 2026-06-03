// SPDX-FileCopyrightText: 2026 @ninsmiracle, @shelly-tang, and LampGo contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LAMPGO_WORK_MODE_H
#define LAMPGO_WORK_MODE_H

namespace WorkMode {

bool connectNetwork();
bool startServices();

void loop();

bool isActive();

}

#endif
